#include "http_parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace http {
namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

Method parse_method(std::string_view m) {
    if (m == "GET") return Method::GET;
    if (m == "HEAD") return Method::HEAD;
    if (m == "POST") return Method::POST;
    if (m == "PUT") return Method::PUT;
    if (m == "DELETE") return Method::DELETE_;
    if (m == "OPTIONS") return Method::OPTIONS;
    if (m == "PATCH") return Method::PATCH;
    return Method::UNKNOWN;
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

ParseResult fail(int code) { return {ParseStatus::Error, 0, code}; }

}  // namespace

std::string_view to_string(Method m) {
    switch (m) {
        case Method::GET: return "GET";
        case Method::HEAD: return "HEAD";
        case Method::POST: return "POST";
        case Method::PUT: return "PUT";
        case Method::DELETE_: return "DELETE";
        case Method::OPTIONS: return "OPTIONS";
        case Method::PATCH: return "PATCH";
        default: return "UNKNOWN";
    }
}

std::string url_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && hex(in[i + 1]) >= 0 && hex(in[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hex(in[i + 1]) * 16 + hex(in[i + 2])));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::optional<std::string_view> Request::header(std::string_view name) const {
    auto it = headers.find(std::string(name));
    if (it == headers.end()) return std::nullopt;
    return std::string_view(it->second);
}

bool Request::keep_alive() const {
    if (auto c = header("connection")) {
        auto v = lower(*c);
        if (v.find("close") != std::string::npos) return false;
        if (v.find("keep-alive") != std::string::npos) return true;
    }
    return version_minor >= 1;
}

Response Response::text(int status, std::string body) {
    Response r;
    r.status = status;
    r.headers["Content-Type"] = "text/plain; charset=utf-8";
    r.body = std::move(body);
    return r;
}

Response Response::json(int status, std::string body) {
    Response r;
    r.status = status;
    r.headers["Content-Type"] = "application/json";
    r.body = std::move(body);
    return r;
}

std::string_view reason_phrase(int s) {
    switch (s) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

std::string serialize(const Response& res, bool keep_alive, bool head_only) {
    std::string out;
    out.reserve(128 + res.body.size());
    out += "HTTP/1.1 ";
    out += std::to_string(res.status);
    out += ' ';
    out += reason_phrase(res.status);
    out += "\r\n";
    for (const auto& [k, v] : res.headers) {
        out += k;
        out += ": ";
        out += v;
        out += "\r\n";
    }
    out += "Content-Length: ";
    out += std::to_string(res.body.size());
    out += "\r\nConnection: ";
    out += keep_alive ? "keep-alive" : "close";
    out += "\r\nServer: asynchttpd\r\n\r\n";
    if (!head_only) out += res.body;
    return out;
}

ParseResult parse_request(std::string_view buf, Request& out, const Limits& lim) {
    constexpr auto npos = std::string_view::npos;

    const size_t end = buf.find("\r\n\r\n");
    if (end == npos) {
        if (buf.size() > lim.max_header) return fail(431);
        return {ParseStatus::Incomplete, 0, 0};
    }
    if (end > lim.max_header) return fail(431);

    const std::string_view head = buf.substr(0, end);
    const size_t body_start = end + 4;

    // Request line
    const size_t eol = head.find("\r\n");
    const std::string_view line = head.substr(0, eol == npos ? head.size() : eol);
    const size_t s1 = line.find(' ');
    if (s1 == npos) return fail(400);
    const size_t s2 = line.find(' ', s1 + 1);
    if (s2 == npos) return fail(400);

    Request req;
    req.method = parse_method(line.substr(0, s1));
    if (req.method == Method::UNKNOWN) return fail(501);

    const std::string_view target = line.substr(s1 + 1, s2 - s1 - 1);
    if (target.empty() || target[0] != '/') return fail(400);

    const std::string_view ver = line.substr(s2 + 1);
    if (ver == "HTTP/1.1") req.version_minor = 1;
    else if (ver == "HTTP/1.0") req.version_minor = 0;
    else return fail(505);

    // Headers
    size_t pos = (eol == npos) ? head.size() : eol + 2;
    while (pos < head.size()) {
        size_t next = head.find("\r\n", pos);
        if (next == npos) next = head.size();
        const std::string_view l = head.substr(pos, next - pos);
        const size_t colon = l.find(':');
        if (colon == npos || colon == 0) return fail(400);
        std::string key = lower(trim(l.substr(0, colon)));
        std::string val(trim(l.substr(colon + 1)));
        auto [it, inserted] = req.headers.try_emplace(std::move(key), val);
        if (!inserted) {
            it->second += ", ";
            it->second += val;
        }
        pos = next + 2;
    }

    if (req.headers.count("transfer-encoding")) return fail(501);  // chunked not supported

    size_t clen = 0;
    if (auto it = req.headers.find("content-length"); it != req.headers.end()) {
        const std::string& v = it->second;
        auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), clen);
        if (ec != std::errc{} || ptr != v.data() + v.size()) return fail(400);
        if (clen > lim.max_body) return fail(413);
    }
    if (buf.size() < body_start + clen) return {ParseStatus::Incomplete, 0, 0};

    req.body.assign(buf.substr(body_start, clen));
    req.target.assign(target);
    const size_t q = target.find('?');
    req.path = url_decode(target.substr(0, q));
    if (q != npos) req.query.assign(target.substr(q + 1));

    out = std::move(req);
    return {ParseStatus::Complete, body_start + clen, 0};
}

}  // namespace http
