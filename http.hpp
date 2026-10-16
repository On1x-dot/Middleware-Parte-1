#pragma once
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace http {

using Headers = std::map<std::string, std::string>;

struct Request {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    Headers headers;
    std::string body;
};

struct Response {
    int status = 200;
    Headers headers;
    std::string body;
};

inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

inline const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 504: return "Gateway Timeout";
        default: return "Status";
    }
}

inline bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

inline void setTimeout(int fd, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

inline Headers parseHeaders(const std::string& head, std::string& firstLine) {
    Headers h;
    size_t pos = head.find("\r\n");
    firstLine = head.substr(0, pos);
    while (pos != std::string::npos) {
        size_t start = pos + 2;
        pos = head.find("\r\n", start);
        std::string line = head.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        h[lower(line.substr(0, colon))] = trim(line.substr(colon + 1));
    }
    return h;
}

inline bool readMessage(int fd, std::string& head, std::string& body) {
    std::string buf;
    char tmp[4096];
    size_t end;
    while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
        if (buf.size() > (1u << 20)) return false;
    }
    head = buf.substr(0, end);
    body = buf.substr(end + 4);
    std::string first;
    Headers h = parseHeaders(head, first);
    size_t want = 0;
    auto it = h.find("content-length");
    if (it != h.end()) want = std::strtoul(it->second.c_str(), nullptr, 10);
    if (want > (8u << 20)) return false;
    while (body.size() < want) {
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        body.append(tmp, static_cast<size_t>(n));
    }
    body.resize(want);
    return true;
}

inline bool parseRequest(const std::string& head, const std::string& body, Request& r) {
    std::string first;
    r.headers = parseHeaders(head, first);
    std::istringstream in(first);
    std::string version;
    if (!(in >> r.method >> r.target >> version)) return false;
    size_t q = r.target.find('?');
    r.path = r.target.substr(0, q);
    r.query = q == std::string::npos ? "" : r.target.substr(q + 1);
    r.body = body;
    return true;
}

inline std::string serialize(const Response& r) {
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
    bool hasType = false;
    for (const auto& [k, v] : r.headers) {
        if (lower(k) == "content-type") hasType = true;
        out += k + ": " + v + "\r\n";
    }
    if (!hasType) out += "Content-Type: application/json\r\n";
    out += "Content-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n" + r.body;
    return out;
}

inline std::optional<Response> call(const std::string& host, int port, const std::string& method,
                                    const std::string& target, const std::string& body,
                                    const Headers& extra = {}, int timeoutSec = 5) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return std::nullopt;
    int fd = -1;
    for (addrinfo* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        setTimeout(fd, timeoutSec);
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return std::nullopt;

    std::string req = method + " " + target + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) + "\r\n";
    for (const auto& [k, v] : extra) req += k + ": " + v + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;

    std::string head, rbody;
    bool ok = sendAll(fd, req) && readMessage(fd, head, rbody);
    close(fd);
    if (!ok) return std::nullopt;

    std::string first;
    Response r;
    r.headers = parseHeaders(head, first);
    std::istringstream in(first);
    std::string version;
    if (!(in >> version >> r.status)) return std::nullopt;
    r.body = rbody;
    return r;
}

using Handler = std::function<Response(const Request&)>;

inline void handleConnection(int fd, const Handler& handler) {
    setTimeout(fd, 10);
    std::string head, body;
    Request req;
    Response res;
    if (readMessage(fd, head, body) && parseRequest(head, body, req)) {
        try {
            res = handler(req);
        } catch (const std::exception&) {
            res = Response{};
            res.status = 500;
            res.body = "{\"error\":\"error interno\"}";
        }
    } else {
        res.status = 400;
        res.body = "{\"error\":\"peticion invalida\"}";
    }
    sendAll(fd, serialize(res));
    shutdown(fd, SHUT_WR);
    close(fd);
}

inline int serve(int port, Handler handler) {
    signal(SIGPIPE, SIG_IGN);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || listen(s, 128) < 0) {
        perror("bind/listen");
        return 1;
    }
    for (;;) {
        int fd = accept(s, nullptr, nullptr);
        if (fd < 0) continue;
        std::thread([fd, handler] { handleConnection(fd, handler); }).detach();
    }
}

}  // namespace http
