#include "bfplayer/dlna_client.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    bfplayer::DlnaHttpUrl url;
    check(
        bfplayer::parse_dlna_http_url(
            "http://192.168.1.20:8200/rootDesc.xml", url),
        "IPv4 DLNA URL parses");
    check(
        url.host == "192.168.1.20" && url.port == 8200 &&
            url.path == "/rootDesc.xml",
        "parsed IPv4 URL fields match");
    check(
        bfplayer::parse_dlna_http_url(
            "http://[fe80::1]:8080/device.xml?x=1", url),
        "IPv6 literal parses");
    check(
        url.host == "fe80::1" && url.port == 8080 &&
            url.path == "/device.xml?x=1",
        "parsed IPv6 URL fields match");
    check(
        !bfplayer::parse_dlna_http_url(
            "http://user:password@nas/device.xml", url),
        "URL credentials are rejected");
    check(
        !bfplayer::parse_dlna_http_url(
            "https://nas/device.xml", url),
        "UPnP control client remains bounded to plain HTTP");
    check(
        bfplayer::resolve_dlna_url(
            "http://nas:8200/a/device.xml", "../not-normalized") ==
            "http://nas:8200/a/../not-normalized",
        "relative reference resolves against the base directory");
    check(
        bfplayer::resolve_dlna_url(
            "http://nas:8200/a/device.xml", "/content/7") ==
            "http://nas:8200/content/7",
        "absolute path resolves against origin");
    check(
        bfplayer::resolve_dlna_url(
            "http://nas:8200/a/device.xml",
            "HtTp://media.example.test/video") ==
            "HtTp://media.example.test/video",
        "absolute references are recognized case insensitively");

    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "LOCATION: \thttp://nas:8200/device.xml \t\r\n"
        "UsN: uuid:test::urn:schemas-upnp-org:device:MediaServer:1\r\n\r\n";
    check(
        bfplayer::dlna_header_value(response, "location") ==
            "http://nas:8200/device.xml",
        "SSDP headers are case insensitive");
    check(
        bfplayer::dlna_header_value(response, "USN").starts_with("uuid:test"),
        "USN is extracted");

    check(
        bfplayer::parse_dlna_duration_us("1:02:03.500") == 3723500000LL,
        "DIDL duration parses");
    check(
        bfplayer::parse_dlna_duration_us("0:60:00") == -1,
        "invalid minutes are rejected");
    check(
        bfplayer::parse_dlna_duration_us("not-a-duration") == -1,
        "malformed duration is rejected");

    std::cout << "dlna_protocol_tests: PASS\n";
    return 0;
}
