#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace http {

enum class Method { GET, HEAD, POST, PUT, DELETE_, OPTIONS, PATCH, UNKNOWN };

std::string_view to_string(Method m);

struct Request {
    Method method = Method::UNKNOWN;
    std::string target;  // raw request-target
    std::string path;    // URL-decoded path
    std::string query;   // raw query string (without '?')
    int version_minor = 1;
    std::unordered_map<std::string, std::string> headers;  // lower-case keys
    std::string body;

    // `name` must be lower-case.
    std::optional<std::string_view> header(std::string_view name) const;
    bool keep_alive() const;
};

struct Response {
    int status = 200;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    static Response text(int status, std::string body);
    static Response json(int status, std::string body);
};

std::string_view reason_phrase(int status);
std::string serialize(const Response& res, bool keep_alive, bool head_only);
std::string url_decode(std::string_view in);

struct Limits {
    size_t max_header = 16 * 1024;
    size_t max_body = 1024 * 1024;
};

enum class ParseStatus { Incomplete, Complete, Error };

struct ParseResult {
    ParseStatus status = ParseStatus::Incomplete;
    size_t consumed = 0;  // bytes used by the request when Complete
    int error_code = 0;   // HTTP status to answer with when Error
};

// Incremental, allocation-light HTTP/1.x request parser.
// Call again with more data while it returns Incomplete.
ParseResult parse_request(std::string_view buf, Request& out, const Limits& limits = {});

}  // namespace http
