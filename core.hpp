#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace core {

template <typename T>
class Repository {
public:
    int add(T item) {
        item.id = next_id_++;
        const int id = item.id;
        items_.emplace(id, std::move(item));
        return id;
    }
    std::optional<T> find(int id) const {
        auto it = items_.find(id);
        if (it == items_.end()) return std::nullopt;
        return it->second;
    }
    bool update(const T& item) {
        auto it = items_.find(item.id);
        if (it == items_.end()) return false;
        it->second = item;
        return true;
    }
    std::vector<T> all() const {
        std::vector<T> out;
        out.reserve(items_.size());
        for (const auto& [_, v] : items_) out.push_back(v);
        return out;
    }

private:
    std::map<int, T> items_;
    int next_id_ = 1;
};

class EventBus {
public:
    using Handler = std::function<void(const std::string& payload)>;
    void subscribe(const std::string& topic, Handler h) { handlers_[topic].push_back(std::move(h)); }
    void publish(const std::string& topic, const std::string& payload) const {
        auto it = handlers_.find(topic);
        if (it == handlers_.end()) return;
        for (const auto& h : it->second) h(payload);
    }

private:
    std::map<std::string, std::vector<Handler>> handlers_;
};

}
