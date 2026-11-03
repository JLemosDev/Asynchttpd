#pragma once
#include <atomic>
#include <cstdint>
#include <string>

#include "router.hpp"

namespace http {

struct Config {
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    unsigned threads = 0;      // 0 = hardware concurrency
    int idle_timeout_s = 30;   // close idle keep-alive connections
    Limits limits{};
};

struct Stats {
    std::atomic<uint64_t> requests{0};
    std::atomic<uint64_t> connections{0};  // accepted in total
    std::atomic<int64_t> active{0};
    std::atomic<uint64_t> parse_errors{0};
};

// Multi-reactor server: one epoll loop per thread, each with its own
// SO_REUSEPORT listening socket (kernel load-balances accepts), edge-triggered
// non-blocking I/O, HTTP/1.1 keep-alive + pipelining, idle timeouts.
// Handlers run on the reactor thread and must not block.
class Server {
public:
    Server(Config cfg, const Router& router) : cfg_(std::move(cfg)), router_(router) {}

    void run();   // blocks until stop() is called
    void stop() { stop_.store(true, std::memory_order_relaxed); }  // async-signal-safe

    const Stats& stats() const { return stats_; }
    const Config& config() const { return cfg_; }

private:
    Config cfg_;
    const Router& router_;
    Stats stats_;
    std::atomic<bool> stop_{false};
};

}  // namespace http
