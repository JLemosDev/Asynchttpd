#include "router.hpp"

#include <algorithm>
#include <exception>

namespace http {
namespace {

std::vector<std::string> split(std::string_view path) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        size_t j = path.find('/', i);
        if (j == std::string_view::npos) j = path.size();
        if (j > i) out.emplace_back(path.substr(i, j - i));
        i = j;
    }
    return out;
}

bool match(const std::vector<std::string>& pattern, const std::vector<std::string>& segs, Params& params) {
    if (pattern.size() != segs.size()) return false;
    Params tmp;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (!pattern[i].empty() && pattern[i][0] == ':') tmp[pattern[i].substr(1)] = segs[i];
        else if (pattern[i] != segs[i]) return false;
    }
    params = std::move(tmp);
    return true;
}

}  // namespace

void Router::add(Method m, std::string_view pattern, Handler h) {
    routes_.push_back({m, split(pattern), std::move(h)});
}

Response Router::dispatch(const Request& req) const {
    const auto segs = split(req.path);
    std::vector<std::string_view> allowed;
    Params params;

    auto run = [&](const Route& r) -> Response {
        try {
            return r.handler(req, params);
        } catch (const std::exception& e) {
            return Response::text(500, std::string("internal error: ") + e.what() + "\n");
        } catch (...) {
            return Response::text(500, "internal error\n");
        }
    };

    for (const auto& r : routes_) {
        if (!match(r.segments, segs, params)) continue;
        if (r.method == req.method) return run(r);
        allowed.push_back(to_string(r.method));
    }
    // HEAD falls back to GET
    if (req.method == Method::HEAD) {
        for (const auto& r : routes_)
            if (r.method == Method::GET && match(r.segments, segs, params)) return run(r);
    }

    if (!allowed.empty()) {
        std::string list;
        for (auto m : allowed) {
            if (!list.empty()) list += ", ";
            list += m;
        }
        Response res = Response::text(405, "method not allowed\n");
        res.headers["Allow"] = list;
        return res;
    }
    return Response::text(404, "not found\n");
}

}  // namespace http
