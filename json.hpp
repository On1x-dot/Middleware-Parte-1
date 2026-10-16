#pragma once
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace json {

inline std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default: o += c;
        }
    }
    return o;
}

inline std::string quote(const std::string& s) { return "\"" + escape(s) + "\""; }

inline std::string money(double v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(2) << v;
    return o.str();
}

inline size_t valueStart(const std::string& body, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t p = body.find(needle);
    if (p == std::string::npos) return std::string::npos;
    p = body.find(':', p + needle.size());
    if (p == std::string::npos) return std::string::npos;
    p++;
    while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p]))) p++;
    return p;
}

inline std::optional<std::string> getString(const std::string& body, const std::string& key) {
    size_t p = valueStart(body, key);
    if (p == std::string::npos || p >= body.size() || body[p] != '"') return std::nullopt;
    std::string out;
    for (size_t i = p + 1; i < body.size(); i++) {
        char c = body[i];
        if (c == '\\' && i + 1 < body.size()) {
            char n = body[++i];
            switch (n) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                default: out += n;
            }
            continue;
        }
        if (c == '"') return out;
        out += c;
    }
    return std::nullopt;
}

inline std::optional<double> getNumber(const std::string& body, const std::string& key) {
    size_t p = valueStart(body, key);
    if (p == std::string::npos || p >= body.size()) return std::nullopt;
    const char* start = body.c_str() + p;
    char* end = nullptr;
    double v = std::strtod(start, &end);
    if (end == start) return std::nullopt;
    return v;
}

}  // namespace json
