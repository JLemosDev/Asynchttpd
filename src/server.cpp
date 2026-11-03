#include "server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace http {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxPendingOut = 1 << 20;  // stop parsing pipelined requests above this
constexpr int kMaxPerBatch = 64;

struct Conn {
    int fd = -1;
    std::string in;
    std::string out;
    size_t out_off = 0;
    bool close_after = false;
    Clock::time_point last{};

    size_t pending() const { return out.size() - out_off; }
};

enum class ReadResult { Ok, Eof, Error };

int make_listener(const Config& cfg) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.port);
    if (::inet_pton(AF_INET, cfg.host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("invalid host: " + cfg.host);
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || ::listen(fd, SOMAXCONN) < 0) {
        int e = errno;
        ::close(fd);
        throw std::runtime_error(std::string("bind/listen: ") + std::strerror(e));
    }
    return fd;
}

class Worker {
public:
    Worker(const Config& cfg, const Router& router, Stats& stats, const std::atomic<bool>& stop, int listen_fd)
        : cfg_(cfg), router_(router), stats_(stats), stop_(stop), listen_fd_(listen_fd) {}

    void run() {
        epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epfd_ < 0) return;

        epoll_event lev{};
        lev.events = EPOLLIN;  // listener is level-triggered
        lev.data.fd = listen_fd_;
        ::epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &lev);

        std::vector<epoll_event> events(256);
        auto last_sweep = Clock::now();

        while (!stop_.load(std::memory_order_relaxed)) {
            int n = ::epoll_wait(epfd_, events.data(), static_cast<int>(events.size()), 200);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                if (fd == listen_fd_) {
                    accept_all();
                } else if (auto it = conns_.find(fd); it != conns_.end()) {
                    on_event(it->second, events[i].events);
                }
            }
            auto now = Clock::now();
            if (now - last_sweep >= std::chrono::seconds(1)) {
                sweep(now);
                last_sweep = now;
            }
        }

        std::vector<int> fds;
        for (auto& [fd, _] : conns_) fds.push_back(fd);
        for (int fd : fds) close_conn(fd);
        ::close(epfd_);
    }

private:
    void accept_all() {
        for (;;) {
            int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR) continue;
                return;  // EAGAIN or resource exhaustion; listener is level-triggered
            }
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
            ev.data.fd = fd;
            if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
                ::close(fd);
                continue;
            }
            Conn c;
            c.fd = fd;
            c.last = Clock::now();
            conns_.emplace(fd, std::move(c));
            stats_.connections.fetch_add(1, std::memory_order_relaxed);
            stats_.active.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void close_conn(int fd) {
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;
        ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        conns_.erase(it);
        stats_.active.fetch_sub(1, std::memory_order_relaxed);
    }

    void sweep(Clock::time_point now) {
        std::vector<int> idle;
        for (auto& [fd, c] : conns_)
            if (now - c.last > std::chrono::seconds(cfg_.idle_timeout_s)) idle.push_back(fd);
        for (int fd : idle) close_conn(fd);
    }

    ReadResult read_all(Conn& c) {
        char buf[16384];
        for (;;) {
            ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
            if (n > 0) {
                c.in.append(buf, static_cast<size_t>(n));
                // A legitimate request never needs more than headers + body.
                if (c.in.size() > cfg_.limits.max_header + cfg_.limits.max_body + 4) return ReadResult::Ok;
                continue;
            }
            if (n == 0) return ReadResult::Eof;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return ReadResult::Ok;
            return ReadResult::Error;
        }
    }

    // Parses and answers as many buffered requests as allowed.
    // Returns true if it stopped early and more work may remain.
    bool process(Conn& c) {
        size_t used = 0;
        int handled = 0;
        bool more = false;

        while (used < c.in.size() && !c.close_after) {
            if (c.pending() > kMaxPendingOut || handled >= kMaxPerBatch) {
                more = true;
                break;
            }
            std::string_view view(c.in.data() + used, c.in.size() - used);
            Request req;
            ParseResult r = parse_request(view, req, cfg_.limits);
            if (r.status == ParseStatus::Incomplete) break;
            if (r.status == ParseStatus::Error) {
                Response res = Response::text(r.error_code, std::string(reason_phrase(r.error_code)) + "\n");
                c.out += serialize(res, false, false);
                stats_.parse_errors.fetch_add(1, std::memory_order_relaxed);
                c.close_after = true;
                used = c.in.size();
                break;
            }
            const bool ka = req.keep_alive();
            Response res = router_.dispatch(req);
            c.out += serialize(res, ka, req.method == Method::HEAD);
            stats_.requests.fetch_add(1, std::memory_order_relaxed);
            ++handled;
            used += r.consumed;
            if (!ka) c.close_after = true;
        }
        if (c.close_after) used = c.in.size();
        c.in.erase(0, used);
        return more;
    }

    // false => fatal socket error
    bool flush(Conn& c) {
        while (c.pending() > 0) {
            ssize_t n = ::send(c.fd, c.out.data() + c.out_off, c.pending(), MSG_NOSIGNAL);
            if (n > 0) {
                c.out_off += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            return false;
        }
        c.out.clear();
        c.out_off = 0;
        return true;
    }

    void on_event(Conn& c, uint32_t ev) {
        const int fd = c.fd;
        if (ev & EPOLLERR) {
            close_conn(fd);
            return;
        }
        bool eof = false;
        if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {
            ReadResult rr = read_all(c);
            if (rr == ReadResult::Error) {
                close_conn(fd);
                return;
            }
            eof = (rr == ReadResult::Eof);
            c.last = Clock::now();
        }
        for (;;) {
            bool more = process(c);
            if (!flush(c)) {
                close_conn(fd);
                return;
            }
            if (!(more && c.pending() == 0)) break;
        }
        if (eof) c.close_after = true;
        if (c.close_after && c.pending() == 0) close_conn(fd);
    }

    const Config& cfg_;
    const Router& router_;
    Stats& stats_;
    const std::atomic<bool>& stop_;
    int listen_fd_;
    int epfd_ = -1;
    std::unordered_map<int, Conn> conns_;
};

}  // namespace

void Server::run() {
    unsigned n = cfg_.threads ? cfg_.threads : std::max(1u, std::thread::hardware_concurrency());

    std::vector<int> listeners;
    try {
        for (unsigned i = 0; i < n; ++i) listeners.push_back(make_listener(cfg_));
    } catch (...) {
        for (int fd : listeners) ::close(fd);
        throw;
    }

    std::vector<std::thread> threads;
    threads.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        threads.emplace_back([this, fd = listeners[i]] {
            Worker w(cfg_, router_, stats_, stop_, fd);
            w.run();
        });
    }
    for (auto& t : threads) t.join();
    for (int fd : listeners) ::close(fd);
}

}  // namespace http
