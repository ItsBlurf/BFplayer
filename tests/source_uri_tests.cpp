#include "ps5mc/source_uri.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    using ps5mc::is_network_uri;
    using ps5mc::is_supported_stream_uri;
    using ps5mc::redact_uri_secrets;
    using ps5mc::uri_has_credentials;
    using ps5mc::uri_has_sensitive_components;

    check(!is_network_uri("/data/media/Movie.mkv"), "local PS5 path is not a URI");
    check(!is_network_uri("C:\\Media\\Movie.mkv"), "Windows path is not a URI");
    check(!is_network_uri("1http://media.test/Movie.mkv"),
          "scheme must begin with an ASCII letter");
    check(is_network_uri("https://media.test/Movie.mkv"), "HTTPS URI recognized");
    check(is_network_uri("rtsp://192.0.2.10/live"), "RTSP URI recognized");
    check(is_supported_stream_uri("HTTPS://media.test/Movie.mkv"),
          "supported scheme check is case insensitive");
    check(is_supported_stream_uri("udp://239.1.2.3:5000"), "UDP stream supported");
    check(!is_supported_stream_uri("smb://server/share/Movie.mkv"),
          "direct SMB must use the websrv proxy");
    check(!is_supported_stream_uri("file:///data/media/Movie.mkv"),
          "file URI is not a direct network stream");
    check(!uri_has_sensitive_components("https://media.test/Movie.mkv"),
          "plain URI is safe to persist");
    check(!uri_has_credentials("https://media.test/path@segment/movie.mkv"),
          "at-sign outside authority is not userinfo");
    check(!uri_has_credentials("https://media.test/movie.mkv?owner=a@b.test"),
          "at-sign in query is not userinfo");

    const std::string credentials = "https://alice:secret@media.test/Movie.mkv";
    check(uri_has_credentials(credentials), "URI userinfo is detected");
    check(uri_has_sensitive_components(credentials), "credentials are sensitive");
    const std::string redacted_credentials = redact_uri_secrets(credentials);
    check(redacted_credentials.find("alice") == std::string::npos,
          "username is removed from logs");
    check(redacted_credentials.find("secret") == std::string::npos,
          "password is removed from logs");
    check(redacted_credentials == "https://<redacted>@media.test/Movie.mkv",
          "authority remains useful after redaction");

    const std::string signed_url =
        "https://media.test/Movie.m3u8?token=secret-value#session-id";
    check(!uri_has_credentials(signed_url), "signed query is not URI userinfo");
    check(uri_has_sensitive_components(signed_url), "query and fragment are sensitive");
    const std::string redacted_signed = redact_uri_secrets(signed_url);
    check(redacted_signed.find("secret-value") == std::string::npos,
          "query value is removed from logs");
    check(redacted_signed.find("session-id") == std::string::npos,
          "fragment is removed from logs");
    check(redacted_signed ==
              "https://media.test/Movie.m3u8?<redacted>#<redacted>",
          "signed URI keeps only non-secret routing information");

    check(redact_uri_secrets("/data/media/Movie.mkv") == "/data/media/Movie.mkv",
          "local path is unchanged");
    std::cout << "source_uri_tests: PASS\n";
    return 0;
}
