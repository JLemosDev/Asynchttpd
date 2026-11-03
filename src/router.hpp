#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "http_parser.hpp"

namespace http {

using Params = std::unordered_map<std::string, std::string>;
using Handler = std::function<Response(const Request&, const Params&)>;

// Segment router. Patterns like "/users/:id/posts". The first registered
// route that matches wins; HEAD falls back to GET; wrong method -> 405.
// Register all routes before the server starts (not thread-safe afterwards).
class Router {
public:
    void add(Method m, std::string_view pattern, Handler h);
    void get(std::string_view p, Handler h) { add(Method::GET, p, std::move(h)); }
    void post(std::string_view p, Handler h) { add(Method::POST, p, std::move(h)); }
    void put(std::string_view p, Handler h) { add(Method::PUT, p, std::move(h)); }
    void del(std::string_view p, Handler h) { add(Method::DELETE_, p, std::move(h)); }

    Response dispatch(const Request& req) const;

private:
    struct Route {
        Method method;
        std::vector<std::string> segments;
        Handler handler;
    };
    std::vector<Route> routes_;
};

}  // namespace http
