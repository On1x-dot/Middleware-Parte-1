#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include "http.hpp"
#include "json.hpp"

namespace {

struct Upstream {
    std::string host;
    int port;
};

struct Route {
    std::vector<Upstream> upstreams;
    std::atomic<unsigned> next{0};
};

using RouteTable = std::map<std::string, std::unique_ptr<Route>>;

RouteTable routes;
std::atomic<unsigned long> requestCounter{0};

std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, sep)) {
        part = http::trim(part);
        if (!part.empty()) out.push_back(part);
    }
    return out;
}

bool loadRoutes(const std::string& spec) {
    for (const auto& entry : split(spec, ';')) {
        size_t eq = entry.find('=');
        if (eq == std::string::npos) return false;
        std::string prefix = http::trim(entry.substr(0, eq));
        auto route = std::make_unique<Route>();
        for (const auto& target : split(entry.substr(eq + 1), ',')) {
            size_t colon = target.rfind(':');
            if (colon == std::string::npos) return false;
            int port = std::atoi(target.substr(colon + 1).c_str());
            if (port <= 0) return false;
            route->upstreams.push_back({target.substr(0, colon), port});
        }
        if (prefix.empty() || route->upstreams.empty()) return false;
        routes[prefix] = std::move(route);
    }
    return !routes.empty();
}

http::Response reply(int status, const std::string& body) {
    http::Response r;
    r.status = status;
    r.body = body;
    return r;
}

std::string routesJson() {
    std::string out = "{";
    bool firstRoute = true;
    for (const auto& [prefix, route] : routes) {
        if (!firstRoute) out += ",";
        firstRoute = false;
        out += json::quote(prefix) + ":[";
        for (size_t i = 0; i < route->upstreams.size(); i++) {
            if (i) out += ",";
            out += json::quote(route->upstreams[i].host + ":" + std::to_string(route->upstreams[i].port));
        }
        out += "]";
    }
    return out + "}";
}

std::string firstSegment(const std::string& path) {
    size_t start = path.find_first_not_of('/');
    if (start == std::string::npos) return "";
    size_t end = path.find('/', start);
    return path.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

http::Response dispatch(const http::Request& req) {
    if (req.path == "/health") return reply(200, "{\"status\":\"ok\",\"service\":\"middleware\"}");
    if (req.path == "/routes") return reply(200, routesJson());

    auto it = routes.find(firstSegment(req.path));
    if (it == routes.end())
        return reply(404, "{\"error\":\"no hay worker para esta ruta\",\"path\":" + json::quote(req.path) + "}");

    Route& route = *it->second;
    const size_t n = route.upstreams.size();
    const unsigned start = route.next.fetch_add(1);

    std::string requestId;
    auto rid = req.headers.find("x-request-id");
    requestId = rid != req.headers.end() ? rid->second : "mw-" + std::to_string(++requestCounter);

    http::Headers forwarded{{"X-Request-Id", requestId}, {"X-Forwarded-By", "middleware"}};
    auto ct = req.headers.find("content-type");
    if (ct != req.headers.end()) forwarded["Content-Type"] = ct->second;

    for (size_t i = 0; i < n; i++) {
        const Upstream& up = route.upstreams[(start + i) % n];
        const std::string label = up.host + ":" + std::to_string(up.port);
        auto t0 = std::chrono::steady_clock::now();
        auto resp = http::call(up.host, up.port, req.method, req.target, req.body, forwarded);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (!resp) {
            std::cout << requestId << " " << req.method << " " << req.target << " -> " << label
                      << " sin respuesta (" << ms << "ms)" << std::endl;
            continue;
        }
        http::Response out;
        out.status = resp->status;
        out.body = resp->body;
        auto type = resp->headers.find("content-type");
        if (type != resp->headers.end()) out.headers["Content-Type"] = type->second;
        auto worker = resp->headers.find("x-worker");
        if (worker != resp->headers.end()) out.headers["X-Worker"] = worker->second;
        out.headers["X-Upstream"] = label;
        out.headers["X-Request-Id"] = requestId;
        std::cout << requestId << " " << req.method << " " << req.target << " -> " << label << " "
                  << resp->status << " (" << ms << "ms)" << std::endl;
        return out;
    }
    return reply(502, "{\"error\":\"worker no disponible\",\"route\":" + json::quote(it->first) + "}");
}

}  // namespace

int main() {
    const int port = std::atoi(envOr("PORT", "8080").c_str());
    const std::string spec = envOr(
        "ROUTES", "users=worker-users:9000;products=worker-catalog:9000;orders=worker-orders:9000");
    if (!loadRoutes(spec)) {
        std::cerr << "ROUTES invalido: " << spec << std::endl;
        return 1;
    }
    std::cout << "middleware escuchando en " << port << " rutas=" << routesJson() << std::endl;
    return http::serve(port, dispatch);
}
