#pragma once
#include <stdexcept>
#include <string>
#include <vector>

#include "core.hpp"

namespace users {
struct User {
    int id = 0;
    std::string name;
    std::string email;
};

class UserService {
public:
    int registerUser(const std::string& name, const std::string& email) {
        if (name.empty() || email.find('@') == std::string::npos)
            throw std::invalid_argument("nombre o email inválido");
        return repo_.add(User{0, name, email});
    }
    std::optional<User> get(int id) const { return repo_.find(id); }
    std::vector<User> list() const { return repo_.all(); }

private:
    core::Repository<User> repo_;
};
}

namespace catalog {
struct Product {
    int id = 0;
    std::string name;
    double price = 0.0;
    int stock = 0;
};

class CatalogService {
public:
    int addProduct(const std::string& name, double price, int stock) {
        if (name.empty() || price < 0 || stock < 0)
            throw std::invalid_argument("producto inválido");
        return repo_.add(Product{0, name, price, stock});
    }
    std::optional<Product> get(int id) const { return repo_.find(id); }
    std::vector<Product> list() const { return repo_.all(); }

    void reserve(int productId, int qty) {
        auto p = repo_.find(productId);
        if (!p) throw std::runtime_error("producto no existe");
        if (qty <= 0 || p->stock < qty) throw std::runtime_error("stock insuficiente");
        p->stock -= qty;
        repo_.update(*p);
    }

private:
    core::Repository<Product> repo_;
};
}

namespace orders {
struct Order {
    int id = 0;
    int userId = 0;
    int productId = 0;
    int qty = 0;
    double total = 0.0;
};

class OrderService {
public:
    OrderService(users::UserService& u, catalog::CatalogService& c, core::EventBus& bus)
        : users_(u), catalog_(c), bus_(bus) {}

    int placeOrder(int userId, int productId, int qty) {
        if (!users_.get(userId)) throw std::runtime_error("usuario no existe");
        auto product = catalog_.get(productId);
        if (!product) throw std::runtime_error("producto no existe");
        catalog_.reserve(productId, qty);
        const int id = repo_.add(Order{0, userId, productId, qty, product->price * qty});
        bus_.publish("order.created", "pedido #" + std::to_string(id));
        return id;
    }
    std::vector<Order> list() const { return repo_.all(); }

private:
    users::UserService& users_;
    catalog::CatalogService& catalog_;
    core::EventBus& bus_;
    core::Repository<Order> repo_;
};
}
