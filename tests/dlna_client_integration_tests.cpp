#include "bfplayer/dlna_client.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string xml_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        case '\'':
            escaped += "&apos;";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

std::string soap_browse_response(
    std::string_view didl,
    std::uint32_t returned,
    std::uint32_t total) {
    return
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope "
        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body>"
        "<u:BrowseResponse "
        "xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
        "<Result>" +
        xml_escape(didl) +
        "</Result>"
        "<NumberReturned>" + std::to_string(returned) +
        "</NumberReturned>"
        "<TotalMatches>" + std::to_string(total) +
        "</TotalMatches>"
        "<UpdateID>1</UpdateID>"
        "</u:BrowseResponse>"
        "</s:Body>"
        "</s:Envelope>";
}

std::string http_response(
    int status,
    std::string_view reason,
    std::string_view body) {
    return "HTTP/1.1 " + std::to_string(status) + " " +
        std::string(reason) +
        "\r\nContent-Type: text/xml; charset=utf-8"
        "\r\nContent-Length: " + std::to_string(body.size()) +
        " \t\r\nConnection: close\r\n\r\n" +
        std::string(body);
}

std::string chunked_http_response(std::string_view body) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/xml; charset=utf-8\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n";
    const std::size_t first = body.size() / 3;
    const std::size_t second = body.size() / 3;
    std::size_t offset = 0;
    for (const std::size_t count :
         {first, second, body.size() - first - second}) {
        std::ostringstream length;
        length << std::hex << count;
        response += length.str() + "\r\n";
        response.append(body.substr(offset, count));
        response += "\r\n";
        offset += count;
    }
    response += "0\r\n\r\n";
    return response;
}

std::size_t request_content_length(std::string_view request) {
    constexpr std::string_view kHeader = "Content-Length:";
    const std::size_t position = request.find(kHeader);
    if (position == std::string_view::npos) {
        return 0;
    }
    std::size_t start = position + kHeader.size();
    while (start < request.size() &&
           (request[start] == ' ' || request[start] == '\t')) {
        ++start;
    }
    std::size_t result = 0;
    while (start < request.size() &&
           request[start] >= '0' && request[start] <= '9') {
        result = result * 10U +
            static_cast<unsigned int>(request[start] - '0');
        ++start;
    }
    return result;
}

std::string receive_request(int descriptor) {
    timeval timeout{};
    timeout.tv_sec = 3;
    (void)setsockopt(
        descriptor,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &timeout,
        sizeof(timeout));

    std::string request;
    char buffer[4096];
    std::size_t expected = 0;
    for (;;) {
        const ssize_t received =
            recv(descriptor, buffer, sizeof(buffer), 0);
        if (received > 0) {
            request.append(
                buffer, static_cast<std::size_t>(received));
            check(
                request.size() <= 1024U * 1024U,
                "test request exceeded its bound");
            const std::size_t header_end =
                request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                if (expected == 0) {
                    expected = header_end + 4 +
                        request_content_length(request);
                }
                if (request.size() >= expected) {
                    return request;
                }
            }
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return request;
    }
}

void send_response(int descriptor, std::string_view response) {
    std::size_t offset = 0;
    while (offset < response.size()) {
        const ssize_t sent = send(
            descriptor,
            response.data() + offset,
            response.size() - offset,
            MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

class FakeHttpServer {
public:
    using Handler = std::function<std::string(
        std::string_view,
        std::size_t,
        std::uint16_t)>;

    FakeHttpServer(
        std::size_t expected_requests,
        Handler handler)
        : expected_requests_(expected_requests),
          handler_(std::move(handler)) {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        check(listener_ >= 0, "fake server socket opens");
        const int one = 1;
        (void)setsockopt(
            listener_,
            SOL_SOCKET,
            SO_REUSEADDR,
            &one,
            sizeof(one));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        check(
            bind(
                listener_,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == 0,
            "fake server binds loopback");
        check(
            listen(listener_, 8) == 0,
            "fake server listens");
        socklen_t address_size = sizeof(address);
        check(
            getsockname(
                listener_,
                reinterpret_cast<sockaddr*>(&address),
                &address_size) == 0,
            "fake server reads assigned port");
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { run(); });
    }

    FakeHttpServer(const FakeHttpServer&) = delete;
    FakeHttpServer& operator=(const FakeHttpServer&) = delete;

    ~FakeHttpServer() {
        stop_.store(true, std::memory_order_relaxed);
        if (listener_ >= 0) {
            (void)shutdown(listener_, SHUT_RDWR);
            close(listener_);
            listener_ = -1;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" +
            std::to_string(port_);
    }

    bool wait_until_accepted(
        std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return accepted_condition_.wait_for(
            lock,
            timeout,
            [this] { return accepted_count_ > 0; });
    }

    void finish() {
        if (thread_.joinable()) {
            thread_.join();
        }
        check(error_.empty(), error_.c_str());
        check(
            requests_.size() == expected_requests_,
            "fake server received the expected request count");
    }

    [[nodiscard]] std::vector<std::string> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    void run() {
        for (std::size_t index = 0;
             index < expected_requests_ &&
             !stop_.load(std::memory_order_relaxed);
             ++index) {
            pollfd state{listener_, POLLIN, 0};
            int poll_result = 0;
            do {
                poll_result = poll(&state, 1, 5000);
            } while (poll_result < 0 && errno == EINTR);
            if (poll_result <= 0 ||
                (state.revents & POLLIN) == 0) {
                error_ = "fake server accept timed out";
                return;
            }
            const int descriptor =
                accept(listener_, nullptr, nullptr);
            if (descriptor < 0) {
                error_ = "fake server accept failed";
                return;
            }
            {
                std::lock_guard lock(mutex_);
                ++accepted_count_;
                accepted_condition_.notify_all();
            }
            const std::string request =
                receive_request(descriptor);
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(request);
            }
            const std::string response =
                handler_(request, index, port_);
            send_response(descriptor, response);
            (void)shutdown(descriptor, SHUT_RDWR);
            close(descriptor);
        }
    }

    int listener_ = -1;
    std::uint16_t port_ = 0;
    std::size_t expected_requests_ = 0;
    Handler handler_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::condition_variable accepted_condition_;
    std::size_t accepted_count_ = 0;
    std::vector<std::string> requests_;
    std::string error_;
};

bfplayer::DlnaServer server_description(
    const FakeHttpServer& server) {
    bfplayer::DlnaServer description;
    description.friendly_name = "Integration NAS";
    description.udn = "uuid:bfplayer-integration";
    description.control_url =
        server.base_url() + "/upnp/control/content";
    return description;
}

std::string didl_header() {
    return
        "<?xml version=\"1.0\"?>"
        "<DIDL-Lite "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
        "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">";
}

void test_paged_chunked_browse() {
    FakeHttpServer server(
        2,
        [](std::string_view request,
           std::size_t index,
           std::uint16_t port) {
            check(
                request.find(
                    "SOAPACTION: "
                    "\"urn:schemas-upnp-org:service:"
                    "ContentDirectory:1#Browse\"") !=
                    std::string_view::npos,
                "browse request includes SOAP action");
            const std::string origin =
                "http://127.0.0.1:" + std::to_string(port);
            if (index == 0) {
                check(
                    request.find(
                        "<StartingIndex>0</StartingIndex>") !=
                        std::string_view::npos,
                    "first browse page starts at zero");
                const std::string didl =
                    didl_header() +
                    "<container id=\"shows\" parentID=\"0\" "
                    "childCount=\"12\">"
                    "<dc:title>Shows &amp; Series</dc:title>"
                    "<upnp:class>"
                    "object.container.storageFolder"
                    "</upnp:class>"
                    "</container>"
                    "<item id=\"episode-1\" parentID=\"0\">"
                    "<dc:title>Episode 1</dc:title>"
                    "<upnp:class>object.item.videoItem</upnp:class>"
                    "<upnp:albumArtURI>/art/episode-1.jpg"
                    "</upnp:albumArtURI>"
                    "<res protocolInfo=\"http-get:*:video/x-matroska:*\" "
                    "duration=\"0:42:03.250\" size=\"123456789\" "
                    "resolution=\"1920x1080\">" +
                    origin + "/media/episode-1.mkv"
                    "</res>"
                    "</item>"
                    "</DIDL-Lite>";
                return chunked_http_response(
                    soap_browse_response(didl, 2, 3));
            }
            check(
                request.find(
                    "<StartingIndex>2</StartingIndex>") !=
                    std::string_view::npos,
                "second browse page follows NumberReturned");
            const std::string didl =
                didl_header() +
                "<item id=\"song-1\" parentID=\"0\">"
                "<dc:title>Song 1</dc:title>"
                "<upnp:class>object.item.audioItem.musicTrack"
                "</upnp:class>"
                "<res protocolInfo=\"http-get:*:audio/flac:*\" "
                "duration=\"0:03:15.500\">media/song-1.flac</res>"
                "</item>"
                "</DIDL-Lite>";
            const std::string body =
                soap_browse_response(didl, 1, 3);
            return http_response(200, "OK", body);
        });

    std::atomic<bool> cancel{false};
    bfplayer::DlnaBrowseResult result;
    std::string error;
    check(
        bfplayer::browse_dlna_directory(
            server_description(server),
            "0",
            10,
            cancel,
            result,
            error),
        "paged DLNA browse succeeds");
    server.finish();
    check(error.empty(), "paged browse has no error");
    check(result.total_matches == 3, "total match count is retained");
    check(!result.truncated, "complete paged browse is not truncated");
    check(result.objects.size() == 3, "all paged objects are returned");
    check(
        result.objects[0].container &&
            result.objects[0].id == "shows" &&
            result.objects[0].child_count == 12 &&
            result.objects[0].title == "Shows & Series",
        "container metadata parses");
    check(
        result.objects[1].resource_url ==
            server.base_url() + "/media/episode-1.mkv" &&
            result.objects[1].artwork_url ==
                server.base_url() + "/art/episode-1.jpg" &&
            result.objects[1].duration_us == 2523250000LL &&
            result.objects[1].size_bytes == 123456789 &&
            result.objects[1].resolution == "1920x1080",
        "video resource metadata parses");
    check(
        result.objects[2].resource_url ==
            server.base_url() + "/upnp/control/media/song-1.flac" &&
            result.objects[2].duration_us == 195500000LL,
        "relative audio resource resolves against control URL");
}

void test_resource_filtering_and_result_cap() {
    FakeHttpServer filtering_server(
        1,
        [](std::string_view,
           std::size_t,
           std::uint16_t port) {
            const std::string origin =
                "http://127.0.0.1:" + std::to_string(port);
            const std::string didl =
                didl_header() +
                "<container parentID=\"0\">"
                "<dc:title>Missing ID</dc:title>"
                "<upnp:class>object.container</upnp:class>"
                "</container>"
                "<item id=\"image\" parentID=\"0\">"
                "<dc:title>Cover</dc:title>"
                "<upnp:class>object.item.imageItem.photo</upnp:class>"
                "<res protocolInfo=\"http-get:*:image/jpeg:*\">" +
                origin + "/cover.jpg</res>"
                "</item>"
                "<item id=\"smb\" parentID=\"0\">"
                "<dc:title>SMB only</dc:title>"
                "<upnp:class>object.item.videoItem</upnp:class>"
                "<res protocolInfo=\"smb-get:*:video/mp4:*\">"
                "smb://nas/share/video.mp4</res>"
                "</item>"
                "<item id=\"credential\" parentID=\"0\">"
                "<dc:title>Credential URL</dc:title>"
                "<upnp:class>object.item.videoItem</upnp:class>"
                "<res protocolInfo=\"http-get:*:video/mp4:*\">"
                "http://user:password@nas/video.mp4</res>"
                "</item>"
                "<item id=\"valid\" parentID=\"0\">"
                "<dc:title>Valid Audio</dc:title>"
                "<upnp:class>object.item.audioItem</upnp:class>"
                "<res protocolInfo=\"http-get:*:audio/mpeg:*\">"
                "/media/valid.mp3</res>"
                "</item>"
                "</DIDL-Lite>";
            const std::string body =
                soap_browse_response(didl, 5, 5);
            return http_response(200, "OK", body);
        });
    std::atomic<bool> cancel{false};
    bfplayer::DlnaBrowseResult filtered;
    std::string error;
    check(
        bfplayer::browse_dlna_directory(
            server_description(filtering_server),
            "0",
            20,
            cancel,
            filtered,
            error),
        "filtering browse succeeds");
    filtering_server.finish();
    check(
        filtered.objects.size() == 1 &&
            filtered.objects[0].id == "valid",
        "malformed, image, SMB, and credential items are filtered");

    FakeHttpServer cap_server(
        1,
        [](std::string_view,
           std::size_t,
           std::uint16_t port) {
            const std::string origin =
                "http://127.0.0.1:" + std::to_string(port);
            std::string didl = didl_header();
            for (int index = 0; index < 3; ++index) {
                didl +=
                    "<item id=\"video-" + std::to_string(index) +
                    "\" parentID=\"0\">"
                    "<dc:title>Video " + std::to_string(index) +
                    "</dc:title>"
                    "<upnp:class>object.item.videoItem</upnp:class>"
                    "<res protocolInfo=\"http-get:*:video/mp4:*\">" +
                    origin + "/video-" + std::to_string(index) +
                    ".mp4</res></item>";
            }
            didl += "</DIDL-Lite>";
            const std::string body =
                soap_browse_response(didl, 3, 100);
            return http_response(200, "OK", body);
        });
    bfplayer::DlnaBrowseResult capped;
    error.clear();
    check(
        bfplayer::browse_dlna_directory(
            server_description(cap_server),
            "0",
            2,
            cancel,
            capped,
            error),
        "bounded browse succeeds");
    cap_server.finish();
    check(
        capped.objects.size() == 2 && capped.truncated,
        "result cap truncates a larger server listing");
}

void test_versioned_service_and_resource_fallback() {
    FakeHttpServer server(
        1,
        [](std::string_view request,
           std::size_t,
           std::uint16_t port) {
            check(
                request.find(
                    "SOAPACTION: "
                    "\"urn:schemas-upnp-org:service:"
                    "ContentDirectory:2#Browse\"") !=
                    std::string_view::npos,
                "browse uses the server's ContentDirectory version");
            check(
                request.find(
                    "xmlns:u=\"urn:schemas-upnp-org:service:"
                    "ContentDirectory:2\"") !=
                    std::string_view::npos,
                "browse body uses the server's service namespace");
            const std::string origin =
                "http://127.0.0.1:" + std::to_string(port);
            const std::string didl =
                didl_header() +
                "<item id=\"fallback\" parentID=\"0\">"
                "<dc:title>Fallback Video</dc:title>"
                "<upnp:class>object.item.videoItem</upnp:class>"
                "<res protocolInfo=\"http-get:*:video/mp4:*\">"
                "http://user:password@nas/rejected.mp4</res>"
                "<res protocolInfo=\"smb-get:*:video/mp4:*\">"
                "smb://nas/share/rejected.mp4</res>"
                "<res protocolInfo=\"HTTP-GET:*:video/mp4:*\" "
                "duration=\"0:01:02.500\"> \n" +
                origin + "/media/fallback.mp4 \n</res>"
                "</item>"
                "<item id=\"typed\" parentID=\"0\">"
                "<dc:title>Protocol-typed Audio</dc:title>"
                "<upnp:class>object.item</upnp:class>"
                "<res protocolInfo=\"http-get:*:audio/flac:*\">"
                "/media/song.flac</res>"
                "</item>"
                "<item id=\"matching\" parentID=\"0\">"
                "<dc:title>Matching Video</dc:title>"
                "<upnp:class>object.item.videoItem</upnp:class>"
                "<res protocolInfo=\"http-get:*:audio/mpeg:*\">"
                "/media/wrong.mp3</res>"
                "<res protocolInfo=\"http-get:*:video/x-matroska:*\" "
                "resolution=\"3840x2160\">"
                "/media/right.mkv</res>"
                "</item>"
                "</DIDL-Lite>";
            return http_response(
                200,
                "OK",
                soap_browse_response(didl, 3, 3));
        });

    bfplayer::DlnaServer description =
        server_description(server);
    description.content_directory_type =
        "urn:schemas-upnp-org:service:ContentDirectory:2";
    std::atomic<bool> cancel{false};
    bfplayer::DlnaBrowseResult result;
    std::string error;
    check(
        bfplayer::browse_dlna_directory(
            description,
            "0",
            10,
            cancel,
            result,
            error),
        "versioned multi-resource browse succeeds");
    server.finish();
    check(error.empty(), "versioned browse has no error");
    check(
        result.objects.size() == 3,
        "generic typed media and valid fallbacks are retained");
    check(
        result.objects[0].resource_url ==
                server.base_url() + "/media/fallback.mp4" &&
            result.objects[0].duration_us == 62500000LL,
        "invalid earlier resources do not hide a valid later stream");
    check(
        result.objects[1].resource_url ==
            server.base_url() + "/media/song.flac",
        "protocol MIME identifies generic media items");
    check(
        result.objects[2].resource_url ==
                server.base_url() + "/media/right.mkv" &&
            result.objects[2].resolution == "3840x2160",
        "resource MIME matching the item class wins");
}

void test_partial_failure_and_cancellation() {
    FakeHttpServer partial_server(
        2,
        [](std::string_view,
           std::size_t index,
           std::uint16_t port) {
            if (index == 1) {
                return http_response(
                    500,
                    "Internal Server Error",
                    "<error/>");
            }
            const std::string didl =
                didl_header() +
                "<item id=\"first\" parentID=\"0\">"
                "<dc:title>First</dc:title>"
                "<upnp:class>object.item.videoItem</upnp:class>"
                "<res protocolInfo=\"http-get:*:video/mp4:*\">"
                "http://127.0.0.1:" + std::to_string(port) +
                "/first.mp4</res></item></DIDL-Lite>";
            const std::string body =
                soap_browse_response(didl, 1, 2);
            return http_response(200, "OK", body);
        });
    std::atomic<bool> cancel{false};
    bfplayer::DlnaBrowseResult partial;
    std::string error;
    check(
        bfplayer::browse_dlna_directory(
            server_description(partial_server),
            "0",
            20,
            cancel,
            partial,
            error),
        "partial results survive a later page failure");
    partial_server.finish();
    check(
        partial.objects.size() == 1 &&
            partial.truncated &&
            error.find("HTTP 500") != std::string::npos,
        "partial browse exposes truncation and its error");

    FakeHttpServer slow_server(
        1,
        [](std::string_view,
           std::size_t,
           std::uint16_t) {
            std::this_thread::sleep_for(1500ms);
            const std::string body =
                soap_browse_response(
                    didl_header() + "</DIDL-Lite>",
                    0,
                    0);
            return http_response(200, "OK", body);
        });
    std::atomic<bool> cancel_slow{false};
    bfplayer::DlnaBrowseResult cancelled_result;
    std::string cancelled_error;
    bool cancelled_success = true;
    const auto started = std::chrono::steady_clock::now();
    std::thread client([&] {
        cancelled_success =
            bfplayer::browse_dlna_directory(
                server_description(slow_server),
                "0",
                20,
                cancel_slow,
                cancelled_result,
                cancelled_error);
    });
    check(
        slow_server.wait_until_accepted(2s),
        "slow browse reaches the server");
    cancel_slow.store(true, std::memory_order_relaxed);
    client.join();
    const auto elapsed =
        std::chrono::steady_clock::now() - started;
    slow_server.finish();
    check(!cancelled_success, "cancelled browse reports failure");
    check(
        cancelled_error == "Cancelled",
        "cancelled browse reports a clear reason");
    check(
        elapsed < 1s,
        "cancellation interrupts the client before server response");
}

} // namespace

int main() {
    test_paged_chunked_browse();
    test_resource_filtering_and_result_cap();
    test_versioned_service_and_resource_fallback();
    test_partial_failure_and_cancellation();
    std::cout << "dlna_client_integration_tests: PASS\n";
    return 0;
}
