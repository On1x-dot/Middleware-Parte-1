#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

#include "http.hpp"
#include "json.hpp"
#include "modules.hpp"

namespace {

struct Upstream {
    std::string host;
    int port;
};

std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

std::vector<std::string> segments(const std::string& path) {
    std::vector<std::string> out;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/'))
        if (!part.empty()) out.push_back(part);
    return out;
}

http::Response reply(int status, const std::string& body) {
    http::Response r;
    r.status = status;
    r.body = body;
    return r;
}

http::Response fail(int status, const std::string& message) {
    return reply(status, "{\"error\":" + json::quote(message) + "}");
}

std::string hostname() {
    char buf[256];
    if (gethostname(buf, sizeof buf) != 0) return "unknown";
    buf[sizeof buf - 1] = '\0';
    return buf;
}

std::optional<int> parseId(const std::string& s) {
    if (s.empty() || s.size() > 9) return std::nullopt;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    return std::stoi(s);
}

std::string userJson(const users::User& u) {
    return "{\"id\":" + std::to_string(u.id) + ",\"name\":" + json::quote(u.name) +
           ",\"email\":" + json::quote(u.email) + "}";
}

std::string productJson(const catalog::Product& p) {
    return "{\"id\":" + std::to_string(p.id) + ",\"name\":" + json::quote(p.name) +
           ",\"price\":" + json::money(p.price) + ",\"stock\":" + std::to_string(p.stock) + "}";
}

std::string orderJson(const orders::Order& o) {
    return "{\"id\":" + std::to_string(o.id) + ",\"userId\":" + std::to_string(o.userId) +
           ",\"productId\":" + std::to_string(o.productId) + ",\"qty\":" + std::to_string(o.qty) +
           ",\"total\":" + json::money(o.total) + "}";
}

template <typename T, typename F>
std::string listJson(const std::vector<T>& items, F render) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); i++) {
        if (i) out += ",";
        out += render(items[i]);
    }
    return out + "]";
}

http::Response handleUsers(users::UserService& svc, const http::Request& req,
                           const std::vector<std::string>& seg) {
    if (seg.empty() || seg[0] != "users") return fail(404, "ruta no encontrada");
    if (seg.size() == 1) {
        if (req.method == "POST") {
            auto name = json::getString(req.body, "name");
            auto email = json::getString(req.body, "email");
            if (!name || !email) return fail(400, "se requieren name y email");
            try {
                int id = svc.registerUser(*name, *email);
                return reply(201, userJson(*svc.get(id)));
            } catch (const std::invalid_argument& e) {
                return fail(400, e.what());
            }
        }
        if (req.method == "GET") return reply(200, listJson(svc.list(), userJson));
        return fail(405, "metodo no permitido");
    }
    if (seg.size() == 2 && req.method == "GET") {
        auto id = parseId(seg[1]);
        if (!id) return fail(400, "id invalido");
        auto u = svc.get(*id);
        if (!u) return fail(404, "usuario no existe");
        return reply(200, userJson(*u));
    }
    return fail(404, "ruta no encontrada");
}

http::Response handleCatalog(catalog::CatalogService& svc, const http::Request& req,
                             const std::vector<std::string>& seg) {
    if (seg.empty() || seg[0] != "products") return fail(404, "ruta no encontrada");
    if (seg.size() == 1) {
        if (req.method == "POST") {
            auto name = json::getString(req.body, "name");
            auto price = json::getNumber(req.body, "price");
            auto stock = json::getNumber(req.body, "stock");
            if (!name || !price || !stock) return fail(400, "se requieren name, price y stock");
            try {
                int id = svc.addProduct(*name, *price, static_cast<int>(*stock));
                return reply(201, productJson(*svc.get(id)));
            } catch (const std::invalid_argument& e) {
                return fail(400, e.what());
            }
        }
        if (req.method == "GET") return reply(200, listJson(svc.list(), productJson));
        return fail(405, "metodo no permitido");
    }
    auto id = parseId(seg[1]);
    if (!id) return fail(400, "id invalido");
    if (seg.size() == 2 && req.method == "GET") {
        auto p = svc.get(*id);
        if (!p) return fail(404, "producto no existe");
        return reply(200, productJson(*p));
    }
    if (seg.size() == 3 && seg[2] == "reserve" && req.method == "POST") {
        auto qty = json::getNumber(req.body, "qty");
        if (!qty) return fail(400, "se requiere qty");
        try {
            svc.reserve(*id, static_cast<int>(*qty));
            return reply(200, productJson(*svc.get(*id)));
        } catch (const std::runtime_error& e) {
            std::string m = e.what();
            return fail(m == "producto no existe" ? 404 : 409, m);
        }
    }
    return fail(404, "ruta no encontrada");
}

http::Response handleOrders(core::Repository<orders::Order>& repo, std::mutex& mu, const Upstream& mw,
                            const http::Request& req, const std::vector<std::string>& seg) {
    if (seg.empty() || seg[0] != "orders") return fail(404, "ruta no encontrada");
    if (seg.size() == 1 && req.method == "GET") {
        std::lock_guard<std::mutex> lock(mu);
        return reply(200, listJson(repo.all(), orderJson));
    }
    if (seg.size() == 2 && req.method == "GET") {
        auto id = parseId(seg[1]);
        if (!id) return fail(400, "id invalido");
        std::lock_guard<std::mutex> lock(mu);
        auto o = repo.find(*id);
        if (!o) return fail(404, "pedido no existe");
        return reply(200, orderJson(*o));
    }
    if (seg.size() == 1 && req.method == "POST") {
        auto userId = json::getNumber(req.body, "userId");
        auto productId = json::getNumber(req.body, "productId");
        auto qty = json::getNumber(req.body, "qty");
        if (!userId || !productId || !qty) return fail(400, "se requieren userId, productId y qty");
        const int u = static_cast<int>(*userId);
        const int p = static_cast<int>(*productId);
        const int q = static_cast<int>(*qty);
        if (q <= 0) return fail(400, "qty debe ser mayor que cero");

        auto user = http::call(mw.host, mw.port, "GET", "/users/" + std::to_string(u), "");
        if (!user) return fail(502, "no se pudo consultar el servicio de usuarios");
        if (user->status == 404) return fail(404, "usuario no existe");
        if (user->status != 200) return fail(502, "respuesta inesperada del servicio de usuarios");

        auto product = http::call(mw.host, mw.port, "POST", "/products/" + std::to_string(p) + "/reserve",
                                  "{\"qty\":" + std::to_string(q) + "}",
                                  {{"Content-Type", "application/json"}});
        if (!product) return fail(502, "no se pudo consultar el servicio de catalogo");
        if (product->status != 200) return reply(product->status, product->body);
        auto price = json::getNumber(product->body, "price");
        if (!price) return fail(502, "respuesta inesperada del servicio de catalogo");

        std::lock_guard<std::mutex> lock(mu);
        int id = repo.add(orders::Order{0, u, p, q, *price * q});
        return reply(201, orderJson(*repo.find(id)));
    }
    return fail(404, "ruta no encontrada");
}

}  // namespace

int main() {
    const std::string role = envOr("ROLE", "users");
    const int port = std::atoi(envOr("PORT", "9000").c_str());
    const Upstream mw{envOr("MIDDLEWARE_HOST", "middleware"), std::atoi(envOr("MIDDLEWARE_PORT", "8080").c_str())};
    if (role != "users" && role != "catalog" && role != "orders") {
        std::cerr << "ROLE invalido: " << role << std::endl;
        return 1;
    }
    const std::string who = role + "@" + hostname();

    users::UserService userSvc;
    catalog::CatalogService catalogSvc;
    core::Repository<orders::Order> orderRepo;
    std::mutex mu;

    std::cout << "worker " << who << " escuchando en " << port << std::endl;
    return http::serve(port, [&](const http::Request& req) {
        http::Response res;
        auto seg = segments(req.path);
        if (req.method == "GET" && req.path == "/health") {
            res = reply(200, "{\"status\":\"ok\",\"role\":" + json::quote(role) + "}");
        } else if (role == "users") {
            std::lock_guard<std::mutex> lock(mu);
            res = handleUsers(userSvc, req, seg);
        } else if (role == "catalog") {
            std::lock_guard<std::mutex> lock(mu);
            res = handleCatalog(catalogSvc, req, seg);
        } else {
            res = handleOrders(orderRepo, mu, mw, req, seg);
        }
        res.headers["X-Worker"] = who;
        return res;
    });
}
