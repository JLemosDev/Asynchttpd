#include <csignal>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "server.hpp"

using namespace http;

namespace {

http::Server* g_server = nullptr;

void on_signal(int) {
    if (g_server) g_server->stop();
}

std::string json_escape(std::string_view s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

// Small thread-safe in-memory store exposed under /kv/:key
class KvStore {
public:
    bool get(const std::string& k, std::string& v) const {
        std::shared_lock lk(m_);
        auto it = map_.find(k);
        if (it == map_.end()) return false;
        v = it->second;
        return true;
    }
    bool put(const std::string& k, std::string v) {  // true if created
        std::unique_lock lk(m_);
        auto [it, inserted] = map_.insert_or_assign(k, std::move(v));
        (void)it;
        return inserted;
    }
    bool erase(const std::string& k) {
        std::unique_lock lk(m_);
        return map_.erase(k) > 0;
    }
    size_t size() const {
        std::shared_lock lk(m_);
        return map_.size();
    }

private:
    mutable std::shared_mutex m_;
    std::unordered_map<std::string, std::string> map_;
};

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << a << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--port") cfg.port = static_cast<uint16_t>(std::stoi(next()));
        else if (a == "--host") cfg.host = next();
        else if (a == "--threads") cfg.threads = static_cast<unsigned>(std::stoi(next()));
        else if (a == "--idle-timeout") cfg.idle_timeout_s = std::stoi(next());
        else if (a == "--help" || a == "-h") {
            std::cout << "usage: httpd [--host H] [--port P] [--threads N] [--idle-timeout S]\n";
            return 0;
        } else {
            std::cerr << "unknown option: " << a << "\n";
            return 2;
        }
    }

    Router router;
    KvStore kv;
    Server server(cfg, router);

    router.get("/", [](const Request&, const Params&) {
        return Response::text(200, "asynchttpd: epoll multi-reactor HTTP server\n");
    });
    router.get("/health", [](const Request&, const Params&) {
        return Response::json(200, R"({"status":"ok"})");
    });
    router.get("/echo/:msg", [](const Request&, const Params& p) {
        return Response::text(200, p.at("msg") + "\n");
    });
    router.post("/echo", [](const Request& req, const Params&) {
        Response r;
        r.status = 200;
        r.headers["Content-Type"] = std::string(req.header("content-type").value_or("application/octet-stream"));
        r.body = req.body;
        return r;
    });
    router.get("/users/:id", [](const Request&, const Params& p) {
        return Response::json(200, "{\"id\":\"" + json_escape(p.at("id")) + "\",\"name\":\"user-" + json_escape(p.at("id")) + "\"}");
    });
    router.get("/kv/:key", [&kv](const Request&, const Params& p) {
        std::string v;
        if (!kv.get(p.at("key"), v)) return Response::text(404, "no such key\n");
        return Response::text(200, v);
    });
    router.put("/kv/:key", [&kv](const Request& req, const Params& p) {
        bool created = kv.put(p.at("key"), req.body);
        return Response::text(created ? 201 : 200, created ? "created\n" : "updated\n");
    });
    router.del("/kv/:key", [&kv](const Request&, const Params& p) {
        return kv.erase(p.at("key")) ? Response::text(204, "") : Response::text(404, "no such key\n");
    });
    router.get("/stats", [&server, &kv](const Request&, const Params&) {
        const Stats& s = server.stats();
        return Response::json(200, "{\"requests\":" + std::to_string(s.requests.load()) +
                                       ",\"connections\":" + std::to_string(s.connections.load()) +
                                       ",\"active\":" + std::to_string(s.active.load()) +
                                       ",\"parse_errors\":" + std::to_string(s.parse_errors.load()) +
                                       ",\"kv_keys\":" + std::to_string(kv.size()) + "}");
    });

    g_server = &server;
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    std::cout << "listening on " << cfg.host << ":" << cfg.port << " (threads="
              << (cfg.threads ? cfg.threads : std::max(1u, std::thread::hardware_concurrency())) << ")\n"
              << std::flush;
    try {
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    std::cout << "shutdown complete, " << server.stats().requests.load() << " requests served\n";
    return 0;
}
