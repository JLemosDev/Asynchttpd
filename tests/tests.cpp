#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include "router.hpp"

using namespace http;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static void test_parser() {
    Request r;
    std::string raw = "GET /a%20b/c?x=1&y=2 HTTP/1.1\r\nHost: x\r\nX-Foo:  bar \r\n\r\n";
    auto res = parse_request(raw, r);
    CHECK(res.status == ParseStatus::Complete);
    CHECK(res.consumed == raw.size());
    CHECK(r.method == Method::GET);
    CHECK(r.path == "/a b/c");
    CHECK(r.query == "x=1&y=2");
    CHECK(r.header("x-foo").value() == "bar");
    CHECK(r.keep_alive());

    // incomplete headers
    CHECK(parse_request("GET / HTTP/1.1\r\nHost: x\r\n", r).status == ParseStatus::Incomplete);

    // body arrives in pieces
    std::string post = "POST /e HTTP/1.1\r\nContent-Length: 5\r\n\r\nhel";
    CHECK(parse_request(post, r).status == ParseStatus::Incomplete);
    post += "lo";
    res = parse_request(post, r);
    CHECK(res.status == ParseStatus::Complete);
    CHECK(r.body == "hello");

    // pipelining: consumed only covers the first request
    std::string two = "GET /1 HTTP/1.1\r\n\r\nGET /2 HTTP/1.1\r\n\r\n";
    res = parse_request(two, r);
    CHECK(res.status == ParseStatus::Complete);
    CHECK(r.path == "/1");
    CHECK(res.consumed == std::string("GET /1 HTTP/1.1\r\n\r\n").size());
    res = parse_request(std::string_view(two).substr(res.consumed), r);
    CHECK(r.path == "/2");

    // errors
    CHECK(parse_request("GARBAGE\r\n\r\n", r).error_code == 400);
    CHECK(parse_request("FOO / HTTP/1.1\r\n\r\n", r).error_code == 501);
    CHECK(parse_request("GET / HTTP/2.0\r\n\r\n", r).error_code == 505);
    CHECK(parse_request("GET x HTTP/1.1\r\n\r\n", r).error_code == 400);
    CHECK(parse_request("POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n", r).error_code == 400);
    CHECK(parse_request("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", r).error_code == 501);
    Limits small{64, 10};
    CHECK(parse_request("POST / HTTP/1.1\r\nContent-Length: 11\r\n\r\n", r, small).error_code == 413);
    CHECK(parse_request(std::string(100, 'a'), r, small).error_code == 431);

    // keep-alive semantics
    parse_request("GET / HTTP/1.0\r\n\r\n", r);
    CHECK(!r.keep_alive());
    parse_request("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", r);
    CHECK(r.keep_alive());
    parse_request("GET / HTTP/1.1\r\nConnection: close\r\n\r\n", r);
    CHECK(!r.keep_alive());
}

static Request make(Method m, std::string path) {
    Request r;
    r.method = m;
    r.path = std::move(path);
    return r;
}

static void test_router() {
    Router rt;
    rt.get("/users/:id", [](const Request&, const Params& p) { return Response::text(200, "u" + p.at("id")); });
    rt.post("/users/:id", [](const Request&, const Params&) { return Response::text(201, "posted"); });
    rt.get("/boom", [](const Request&, const Params&) -> Response { throw std::runtime_error("x"); });

    CHECK(rt.dispatch(make(Method::GET, "/users/42")).body == "u42");
    CHECK(rt.dispatch(make(Method::GET, "/users/42/")).body == "u42");
    CHECK(rt.dispatch(make(Method::POST, "/users/1")).status == 201);
    CHECK(rt.dispatch(make(Method::GET, "/nope")).status == 404);
    CHECK(rt.dispatch(make(Method::GET, "/users")).status == 404);

    auto r405 = rt.dispatch(make(Method::DELETE_, "/users/1"));
    CHECK(r405.status == 405);
    CHECK(r405.headers["Allow"] == "GET, POST");

    CHECK(rt.dispatch(make(Method::HEAD, "/users/7")).status == 200);
    CHECK(rt.dispatch(make(Method::GET, "/boom")).status == 500);
}

static void test_serialize() {
    Response r = Response::text(200, "hi");
    std::string s = serialize(r, true, false);
    CHECK(s.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(s.find("Content-Length: 2\r\n") != std::string::npos);
    CHECK(s.find("Connection: keep-alive\r\n") != std::string::npos);
    CHECK(s.size() >= 2 && s.substr(s.size() - 2) == "hi");
    std::string h = serialize(r, false, true);
    CHECK(h.find("Connection: close") != std::string::npos);
    CHECK(h.find("\r\n\r\nhi") == std::string::npos);
    CHECK(url_decode("a%2Fb+c%zz%") == "a/b+c%zz%");
}

int main() {
    test_parser();
    test_router();
    test_serialize();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all tests passed\n";
    return 0;
}
