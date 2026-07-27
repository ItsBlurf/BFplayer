#include "bfplayer/standalone_route.h"

#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* description) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

} // namespace

int main() {
    check(
        bfplayer_request_is_launch("GET /launch HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"),
        "exact launch route accepted");
    check(!bfplayer_request_is_launch(nullptr), "null rejected");
    check(!bfplayer_request_is_launch(""), "empty request rejected");
    check(
        !bfplayer_request_is_launch("POST /launch HTTP/1.1\r\n\r\n"),
        "alternate method rejected");
    check(
        !bfplayer_request_is_launch("GET /launch?path=/tmp/a HTTP/1.1\r\n\r\n"),
        "query string rejected");
    check(
        !bfplayer_request_is_launch("GET /launcher HTTP/1.1\r\n\r\n"),
        "suffix path rejected");
    check(
        !bfplayer_request_is_launch("GET / HTTP/1.1\r\n\r\n"),
        "other route rejected");
    check(
        bfplayer_request_is_shutdown(
            "GET /shutdown HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"),
        "exact shutdown route accepted");
    check(
        !bfplayer_request_is_shutdown("POST /shutdown HTTP/1.1\r\n\r\n"),
        "shutdown alternate method rejected");
    check(
        !bfplayer_request_is_shutdown(
            "GET /shutdown?force=1 HTTP/1.1\r\n\r\n"),
        "shutdown query string rejected");

    if (failures == 0) {
        std::cout << "standalone_route_tests: PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
