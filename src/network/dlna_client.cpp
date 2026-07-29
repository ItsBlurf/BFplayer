#include "bfplayer/dlna_client.hpp"

#include "bfplayer/source_uri.hpp"

#include <tinyxml2.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bfplayer {
namespace {

using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

constexpr std::size_t kMaxHttpResponseBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaxHttpHeaderBytes = 64U * 1024U;
constexpr std::size_t kMaxSsdpResponses = 32;
constexpr std::size_t kMaxDescribedServers = 16;
constexpr int kHttpTimeoutMs = 6000;
constexpr int kDescriptionRequestTimeoutMs = 3000;
constexpr int kDescriptionBudgetMs = 10000;

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

std::int64_t monotonic_milliseconds() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::string lower_ascii(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string trim_http_value(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           (value[end - 1] == ' ' || value[end - 1] == '\t')) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

int remaining_timeout(std::int64_t deadline) noexcept {
    const std::int64_t remaining = deadline - monotonic_milliseconds();
    if (remaining <= 0) {
        return 0;
    }
    return static_cast<int>(std::min<std::int64_t>(remaining, INT_MAX));
}

bool wait_for_fd(
    int descriptor,
    short events,
    std::int64_t deadline,
    std::atomic<bool>& cancel) {
    while (!cancel.load(std::memory_order_relaxed)) {
        const int remaining = remaining_timeout(deadline);
        if (remaining <= 0) {
            return false;
        }
        pollfd descriptor_state{descriptor, events, 0};
        const int result = poll(
            &descriptor_state,
            1,
            std::min(remaining, 200));
        if (result > 0) {
            return (descriptor_state.revents &
                    (events | POLLERR | POLLHUP)) != 0;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
    }
    return false;
}

int connect_tcp(
    const DlnaHttpUrl& url,
    std::int64_t deadline,
    std::atomic<bool>& cancel,
    std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const std::string port = std::to_string(url.port);
    addrinfo* addresses = nullptr;
    const int resolve_result = getaddrinfo(
        url.host.c_str(), port.c_str(), &hints, &addresses);
    if (resolve_result != 0 || !addresses) {
        error = "Unable to resolve " + url.host;
        return -1;
    }

    int connected = -1;
    for (addrinfo* address = addresses;
         address && !cancel.load(std::memory_order_relaxed);
         address = address->ai_next) {
        int descriptor = socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol);
        if (descriptor < 0) {
            continue;
        }
        const int descriptor_flags = fcntl(descriptor, F_GETFD, 0);
        if (descriptor_flags >= 0) {
            (void)fcntl(
                descriptor,
                F_SETFD,
                descriptor_flags | FD_CLOEXEC);
        }
        const int status_flags = fcntl(descriptor, F_GETFL, 0);
        if (status_flags < 0 ||
            fcntl(
                descriptor,
                F_SETFL,
                status_flags | O_NONBLOCK) != 0) {
            close(descriptor);
            continue;
        }

        if (connect(
                descriptor,
                address->ai_addr,
                address->ai_addrlen) == 0) {
            connected = descriptor;
            break;
        }
        if (errno == EINPROGRESS &&
            wait_for_fd(
                descriptor,
                POLLOUT,
                deadline,
                cancel)) {
            int socket_error = 0;
            socklen_t error_size = sizeof(socket_error);
            if (getsockopt(
                    descriptor,
                    SOL_SOCKET,
                    SO_ERROR,
                    &socket_error,
                    &error_size) == 0 &&
                socket_error == 0) {
                connected = descriptor;
                break;
            }
        }
        close(descriptor);
    }
    freeaddrinfo(addresses);

    if (connected < 0) {
        error = cancel.load(std::memory_order_relaxed)
            ? "Cancelled"
            : "Unable to connect to " + url.host + ":" + port;
        return -1;
    }
    const int one = 1;
    (void)setsockopt(
        connected,
        IPPROTO_TCP,
        TCP_NODELAY,
        &one,
        sizeof(one));
    return connected;
}

bool send_all(
    int descriptor,
    const std::string& data,
    std::int64_t deadline,
    std::atomic<bool>& cancel) {
    std::size_t offset = 0;
    while (offset < data.size() &&
           !cancel.load(std::memory_order_relaxed)) {
        if (!wait_for_fd(
                descriptor,
                POLLOUT,
                deadline,
                cancel)) {
            return false;
        }
        const ssize_t sent = send(
            descriptor,
            data.data() + offset,
            data.size() - offset,
            MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
        } else if (sent < 0 &&
                   (errno == EAGAIN || errno == EWOULDBLOCK ||
                    errno == EINTR)) {
            continue;
        } else {
            return false;
        }
    }
    return offset == data.size();
}

bool parse_content_length(
    std::string_view value,
    std::size_t& output) noexcept {
    if (value.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const unsigned char character : value) {
        if (std::isdigit(character) == 0) {
            return false;
        }
        parsed = parsed * 10U +
            static_cast<unsigned int>(character - '0');
        if (parsed > kMaxHttpResponseBytes) {
            return false;
        }
    }
    output = static_cast<std::size_t>(parsed);
    return true;
}

bool decode_chunked(
    std::string_view input,
    std::string& output,
    std::string& error) {
    output.clear();
    std::size_t position = 0;
    while (position < input.size()) {
        const std::size_t line_end = input.find("\r\n", position);
        if (line_end == std::string_view::npos) {
            error = "Malformed chunked HTTP response";
            return false;
        }
        std::string_view length_text =
            input.substr(position, line_end - position);
        const std::size_t extension = length_text.find(';');
        if (extension != std::string_view::npos) {
            length_text = length_text.substr(0, extension);
        }
        if (length_text.empty() || length_text.size() > 16) {
            error = "Invalid HTTP chunk length";
            return false;
        }
        std::uint64_t chunk_size = 0;
        for (const unsigned char character : length_text) {
            unsigned int digit = 0;
            if (character >= '0' && character <= '9') {
                digit = character - '0';
            } else if (character >= 'a' && character <= 'f') {
                digit = character - 'a' + 10;
            } else if (character >= 'A' && character <= 'F') {
                digit = character - 'A' + 10;
            } else {
                error = "Invalid HTTP chunk length";
                return false;
            }
            chunk_size = chunk_size * 16U + digit;
            if (chunk_size > kMaxHttpResponseBytes) {
                error = "HTTP response is too large";
                return false;
            }
        }
        position = line_end + 2;
        if (chunk_size == 0) {
            return true;
        }
        if (chunk_size > input.size() - position ||
            output.size() >
                kMaxHttpResponseBytes -
                    static_cast<std::size_t>(chunk_size)) {
            error = "Incomplete chunked HTTP response";
            return false;
        }
        output.append(
            input.substr(
                position,
                static_cast<std::size_t>(chunk_size)));
        position += static_cast<std::size_t>(chunk_size);
        if (position + 2 > input.size() ||
            input.substr(position, 2) != "\r\n") {
            error = "Malformed chunk boundary";
            return false;
        }
        position += 2;
    }
    error = "Incomplete chunked HTTP response";
    return false;
}

bool http_request(
    const std::string& method,
    const std::string& url_text,
    const std::string& extra_headers,
    const std::string& body,
    int timeout_ms,
    std::atomic<bool>& cancel,
    HttpResponse& response,
    std::string& error) {
    DlnaHttpUrl url;
    if (!parse_dlna_http_url(url_text, url)) {
        error = "Unsupported UPnP URL";
        return false;
    }
    const std::int64_t deadline =
        monotonic_milliseconds() +
        std::clamp(timeout_ms, 250, kHttpTimeoutMs);
    const int descriptor =
        connect_tcp(url, deadline, cancel, error);
    if (descriptor < 0) {
        return false;
    }

    std::string request =
        method + " " + url.path + " HTTP/1.1\r\n";
    request += "Host: " + url.host + ":" +
        std::to_string(url.port) + "\r\n";
    request += "User-Agent: BFplayer/1.0 UPnP/1.1\r\n";
    request += "Accept: text/xml, application/xml, */*\r\n";
    request += "Connection: close\r\n";
    if (!body.empty()) {
        request += "Content-Length: " +
            std::to_string(body.size()) + "\r\n";
    }
    if (!extra_headers.empty()) {
        request += extra_headers;
        if (!request.ends_with("\r\n")) {
            request += "\r\n";
        }
    }
    request += "\r\n";
    request += body;

    if (!send_all(descriptor, request, deadline, cancel)) {
        close(descriptor);
        error = cancel.load(std::memory_order_relaxed)
            ? "Cancelled"
            : "UPnP request send failed";
        return false;
    }

    std::string raw;
    raw.reserve(64U * 1024U);
    std::size_t header_end = std::string::npos;
    std::size_t expected_body = 0;
    bool has_content_length = false;
    bool chunked = false;
    char buffer[16384];
    for (;;) {
        if (!wait_for_fd(
                descriptor,
                POLLIN,
                deadline,
                cancel)) {
            close(descriptor);
            error = cancel.load(std::memory_order_relaxed)
                ? "Cancelled"
                : "UPnP response timed out";
            return false;
        }
        const ssize_t received =
            recv(descriptor, buffer, sizeof(buffer), 0);
        if (received > 0) {
            if (raw.size() >
                kMaxHttpResponseBytes -
                    static_cast<std::size_t>(received)) {
                close(descriptor);
                error = "UPnP response is too large";
                return false;
            }
            raw.append(buffer, static_cast<std::size_t>(received));
            if (header_end == std::string::npos) {
                header_end = raw.find("\r\n\r\n");
                if (header_end == std::string::npos &&
                    raw.size() > kMaxHttpHeaderBytes) {
                    close(descriptor);
                    error = "UPnP response headers are too large";
                    return false;
                }
                if (header_end != std::string::npos) {
                    const std::string_view headers(
                        raw.data(), header_end);
                    const std::string length =
                        dlna_header_value(headers, "Content-Length");
                    has_content_length =
                        parse_content_length(length, expected_body);
                    const std::string transfer =
                        lower_ascii(dlna_header_value(
                            headers, "Transfer-Encoding"));
                    chunked =
                        transfer.find("chunked") != std::string::npos;
                }
            }
            if (header_end != std::string::npos &&
                has_content_length &&
                raw.size() >= header_end + 4 + expected_body) {
                break;
            }
            if (header_end != std::string::npos && chunked &&
                raw.find("\r\n0\r\n\r\n", header_end + 4) !=
                    std::string::npos) {
                break;
            }
            continue;
        }
        if (received == 0) {
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK ||
            errno == EINTR) {
            continue;
        }
        close(descriptor);
        error = "UPnP response receive failed";
        return false;
    }
    close(descriptor);

    header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        error = "Malformed UPnP HTTP response";
        return false;
    }
    const std::size_t status_end = raw.find("\r\n");
    if (status_end == std::string::npos ||
        !std::string_view(raw).starts_with("HTTP/")) {
        error = "Malformed UPnP HTTP status";
        return false;
    }
    const std::size_t status_space = raw.find(' ');
    if (status_space == std::string::npos ||
        status_space >= status_end) {
        error = "Malformed UPnP HTTP status";
        return false;
    }
    response = {};
    response.status = std::atoi(raw.c_str() + status_space + 1);

    std::size_t position = status_end + 2;
    while (position < header_end) {
        std::size_t line_end = raw.find("\r\n", position);
        if (line_end == std::string::npos ||
            line_end > header_end) {
            line_end = header_end;
        }
        const std::string_view line(
            raw.data() + position,
            line_end - position);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string key =
                lower_ascii(std::string(line.substr(0, colon)));
            std::size_t value_start = colon + 1;
            while (value_start < line.size() &&
                   (line[value_start] == ' ' ||
                    line[value_start] == '\t')) {
                ++value_start;
            }
            response.headers[std::move(key)] =
                trim_http_value(line.substr(value_start));
        }
        position = line_end + 2;
    }

    std::string_view payload(
        raw.data() + header_end + 4,
        raw.size() - header_end - 4);
    const auto transfer =
        response.headers.find("transfer-encoding");
    if (transfer != response.headers.end() &&
        lower_ascii(transfer->second).find("chunked") !=
            std::string::npos) {
        return decode_chunked(payload, response.body, error);
    }
    const auto length = response.headers.find("content-length");
    if (length != response.headers.end()) {
        std::size_t parsed_length = 0;
        if (!parse_content_length(
                length->second, parsed_length) ||
            payload.size() < parsed_length) {
            error = "Incomplete UPnP HTTP response";
            return false;
        }
        payload = payload.substr(0, parsed_length);
    }
    response.body.assign(payload);
    return true;
}

bool name_is(
    const XMLElement* element,
    const char* local_name) noexcept {
    const char* name = element ? element->Name() : nullptr;
    if (!name) {
        return false;
    }
    const char* colon = std::strchr(name, ':');
    return std::strcmp(
        colon ? colon + 1 : name,
        local_name) == 0;
}

const XMLElement* first_child_local(
    const XMLElement* parent,
    const char* local_name) noexcept {
    if (!parent) {
        return nullptr;
    }
    for (const XMLElement* child = parent->FirstChildElement();
         child;
         child = child->NextSiblingElement()) {
        if (name_is(child, local_name)) {
            return child;
        }
    }
    return nullptr;
}

std::string child_text(
    const XMLElement* parent,
    const char* local_name) {
    const XMLElement* child =
        first_child_local(parent, local_name);
    const char* text = child ? child->GetText() : nullptr;
    return text ? text : "";
}

const XMLElement* find_media_server_device(
    const XMLElement* device) noexcept {
    if (!device) {
        return nullptr;
    }
    if (child_text(device, "deviceType").find(
            ":device:MediaServer:") != std::string::npos) {
        return device;
    }
    const XMLElement* list =
        first_child_local(device, "deviceList");
    for (const XMLElement* child =
             list ? list->FirstChildElement() : nullptr;
         child;
         child = child->NextSiblingElement()) {
        if (name_is(child, "device")) {
            if (const XMLElement* found =
                    find_media_server_device(child)) {
                return found;
            }
        }
    }
    return nullptr;
}

std::string xml_escape(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '&':
            output += "&amp;";
            break;
        case '<':
            output += "&lt;";
            break;
        case '>':
            output += "&gt;";
            break;
        case '"':
            output += "&quot;";
            break;
        case '\'':
            output += "&apos;";
            break;
        default:
            output += character;
            break;
        }
    }
    return output;
}

bool describe_server(
    const std::string& location,
    int timeout_ms,
    std::atomic<bool>& cancel,
    DlnaServer& output,
    std::string& error) {
    HttpResponse response;
    if (!http_request(
            "GET",
            location,
            {},
            {},
            timeout_ms,
            cancel,
            response,
            error)) {
        return false;
    }
    if (response.status != 200) {
        error = "Device description returned HTTP " +
            std::to_string(response.status);
        return false;
    }
    XMLDocument document;
    if (document.Parse(
            response.body.c_str(),
            response.body.size()) != tinyxml2::XML_SUCCESS) {
        error = "Malformed DLNA device description";
        return false;
    }
    const XMLElement* root = document.RootElement();
    const XMLElement* device = find_media_server_device(
        first_child_local(root, "device"));
    if (!device) {
        error = "The discovered device is not a media server";
        return false;
    }

    std::string base = child_text(root, "URLBase");
    if (base.empty()) {
        base = location;
    }
    DlnaServer server;
    server.location = location;
    server.friendly_name = child_text(device, "friendlyName");
    server.udn = child_text(device, "UDN");
    const std::string manufacturer =
        child_text(device, "manufacturer");
    const std::string model = child_text(device, "modelName");
    server.model =
        manufacturer.empty()
            ? model
            : (model.empty()
                   ? manufacturer
                   : manufacturer + " " + model);
    if (server.friendly_name.empty()) {
        server.friendly_name =
            server.model.empty() ? "Media server" : server.model;
    }

    const XMLElement* services =
        first_child_local(device, "serviceList");
    for (const XMLElement* service =
             services ? services->FirstChildElement() : nullptr;
         service;
         service = service->NextSiblingElement()) {
        if (!name_is(service, "service") ||
            child_text(service, "serviceType").find(
                ":service:ContentDirectory:") ==
                std::string::npos) {
            continue;
        }
        const std::string control =
            child_text(service, "controlURL");
        if (!control.empty()) {
            server.control_url =
                resolve_dlna_url(base, control);
            break;
        }
    }
    DlnaHttpUrl validated_control;
    if (server.control_url.empty() ||
        !parse_dlna_http_url(
            server.control_url, validated_control)) {
        error =
            "The media server has no usable content directory";
        return false;
    }
    output = std::move(server);
    return true;
}

DlnaObject parse_didl_object(
    const XMLElement* element,
    bool container,
    const std::string& base_url) {
    DlnaObject object;
    object.container = container;
    if (const char* id = element->Attribute("id")) {
        object.id = id;
    }
    if (const char* parent = element->Attribute("parentID")) {
        object.parent_id = parent;
    }
    object.title = child_text(element, "title");
    object.upnp_class = child_text(element, "class");
    if (container) {
        object.child_count =
            element->IntAttribute("childCount", -1);
        return object;
    }

    std::string art = child_text(element, "albumArtURI");
    if (!art.empty()) {
        object.artwork_url =
            resolve_dlna_url(base_url, art);
    }
    const XMLElement* selected = nullptr;
    const XMLElement* fallback = nullptr;
    for (const XMLElement* resource =
             element->FirstChildElement();
         resource;
         resource = resource->NextSiblingElement()) {
        if (!name_is(resource, "res")) {
            continue;
        }
        const char* resource_text = resource->GetText();
        if (!resource_text || !resource_text[0]) {
            continue;
        }
        const char* protocol =
            resource->Attribute("protocolInfo");
        if (protocol && std::strstr(protocol, ":image/")) {
            if (object.artwork_url.empty()) {
                object.artwork_url =
                    resolve_dlna_url(base_url, resource_text);
            }
            continue;
        }
        if (!fallback) {
            fallback = resource;
        }
        if (!selected && protocol &&
            std::strncmp(protocol, "http-get", 8) == 0) {
            selected = resource;
        }
    }
    if (!selected) {
        selected = fallback;
    }
    if (!selected) {
        return object;
    }
    object.resource_url = resolve_dlna_url(
        base_url,
        selected->GetText() ? selected->GetText() : "");
    if (!is_supported_stream_uri(object.resource_url) ||
        uri_has_credentials(object.resource_url)) {
        object.resource_url.clear();
        return object;
    }
    if (const char* protocol =
            selected->Attribute("protocolInfo")) {
        object.protocol_info = protocol;
    }
    if (const char* duration =
            selected->Attribute("duration")) {
        object.duration_us =
            parse_dlna_duration_us(duration);
    }
    if (const char* size = selected->Attribute("size")) {
        char* end = nullptr;
        const long long parsed = std::strtoll(size, &end, 10);
        if (end && end != size && *end == '\0' && parsed >= 0) {
            object.size_bytes = parsed;
        }
    }
    if (const char* resolution =
            selected->Attribute("resolution")) {
        object.resolution = resolution;
    }
    return object;
}

bool browse_page(
    const DlnaServer& server,
    const std::string& object_id,
    std::uint32_t start,
    std::uint32_t count,
    std::atomic<bool>& cancel,
    DlnaBrowseResult& output,
    std::uint32_t& returned,
    std::string& error) {
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
        " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
        "<ObjectID>" + xml_escape(object_id) + "</ObjectID>"
        "<BrowseFlag>BrowseDirectChildren</BrowseFlag>"
        "<Filter>*</Filter>"
        "<StartingIndex>" + std::to_string(start) + "</StartingIndex>"
        "<RequestedCount>" + std::to_string(count) + "</RequestedCount>"
        "<SortCriteria></SortCriteria>"
        "</u:Browse>"
        "</s:Body>"
        "</s:Envelope>";
    const std::string headers =
        "Content-Type: text/xml; charset=\"utf-8\"\r\n"
        "SOAPACTION: \"urn:schemas-upnp-org:service:ContentDirectory:1#Browse\"\r\n";

    HttpResponse response;
    if (!http_request(
            "POST",
            server.control_url,
            headers,
            body,
            kHttpTimeoutMs,
            cancel,
            response,
            error)) {
        return false;
    }
    if (response.status != 200) {
        error = "DLNA browse returned HTTP " +
            std::to_string(response.status);
        return false;
    }
    XMLDocument envelope;
    if (envelope.Parse(
            response.body.c_str(),
            response.body.size()) != tinyxml2::XML_SUCCESS) {
        error = "Malformed DLNA browse response";
        return false;
    }
    const XMLElement* soap_body =
        first_child_local(envelope.RootElement(), "Body");
    const XMLElement* browse_response =
        first_child_local(soap_body, "BrowseResponse");
    if (!browse_response) {
        error = "Unexpected DLNA browse response";
        return false;
    }
    const std::string didl_text =
        child_text(browse_response, "Result");
    output.total_matches = static_cast<std::uint32_t>(
        std::strtoul(
            child_text(
                browse_response,
                "TotalMatches").c_str(),
            nullptr,
            10));
    const std::uint32_t reported =
        static_cast<std::uint32_t>(
            std::strtoul(
                child_text(
                    browse_response,
                    "NumberReturned").c_str(),
                nullptr,
                10));
    if (didl_text.empty()) {
        returned = reported;
        return reported == 0;
    }

    XMLDocument didl;
    if (didl.Parse(
            didl_text.c_str(),
            didl_text.size()) != tinyxml2::XML_SUCCESS ||
        !didl.RootElement()) {
        error = "Malformed DIDL-Lite listing";
        return false;
    }
    std::uint32_t parsed = 0;
    for (const XMLElement* element =
             didl.RootElement()->FirstChildElement();
         element;
         element = element->NextSiblingElement()) {
        const bool container = name_is(element, "container");
        if (!container && !name_is(element, "item")) {
            continue;
        }
        DlnaObject object = parse_didl_object(
            element,
            container,
            server.control_url);
        ++parsed;
        const bool media_item =
            object.upnp_class.starts_with(
                "object.item.videoItem") ||
            object.upnp_class.starts_with(
                "object.item.audioItem");
        if (object.container ||
            (media_item && object.playable())) {
            if (object.title.empty()) {
                object.title =
                    object.container ? "Folder" : "Untitled media";
            }
            output.objects.push_back(std::move(object));
        }
    }
    returned = std::max(parsed, reported);
    return true;
}

struct SsdpCandidate {
    std::string location;
    std::string usn;
};

std::vector<int> open_ssdp_sockets() {
    std::vector<int> descriptors;
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        interfaces = nullptr;
    }
    std::set<std::uint32_t> seen_addresses;
    for (ifaddrs* interface = interfaces;
         interface;
         interface = interface->ifa_next) {
        if (!interface->ifa_addr ||
            interface->ifa_addr->sa_family != AF_INET ||
            (interface->ifa_flags & IFF_UP) == 0 ||
            (interface->ifa_flags & IFF_LOOPBACK) != 0 ||
            (interface->ifa_flags & IFF_MULTICAST) == 0) {
            continue;
        }
        const auto* address =
            reinterpret_cast<const sockaddr_in*>(
                interface->ifa_addr);
        if (!seen_addresses.insert(
                address->sin_addr.s_addr).second) {
            continue;
        }
        const int descriptor =
            socket(AF_INET, SOCK_DGRAM, 0);
        if (descriptor < 0) {
            continue;
        }
        const int descriptor_flags =
            fcntl(descriptor, F_GETFD, 0);
        if (descriptor_flags >= 0) {
            (void)fcntl(
                descriptor,
                F_SETFD,
                descriptor_flags | FD_CLOEXEC);
        }
        if (setsockopt(
                descriptor,
                IPPROTO_IP,
                IP_MULTICAST_IF,
                &address->sin_addr,
                sizeof(address->sin_addr)) != 0 ||
            bind(
                descriptor,
                interface->ifa_addr,
                sizeof(sockaddr_in)) != 0) {
            close(descriptor);
            continue;
        }
        const unsigned char ttl = 2;
        (void)setsockopt(
            descriptor,
            IPPROTO_IP,
            IP_MULTICAST_TTL,
            &ttl,
            sizeof(ttl));
        descriptors.push_back(descriptor);
    }
    if (interfaces) {
        freeifaddrs(interfaces);
    }
    if (descriptors.empty()) {
        const int descriptor =
            socket(AF_INET, SOCK_DGRAM, 0);
        if (descriptor >= 0) {
            descriptors.push_back(descriptor);
        }
    }
    return descriptors;
}

std::vector<SsdpCandidate> search_ssdp(
    int wait_ms,
    std::atomic<bool>& cancel,
    std::string& error) {
    constexpr const char* kAddress = "239.255.255.250";
    constexpr std::uint16_t kPort = 1900;
    const std::string target =
        "urn:schemas-upnp-org:device:MediaServer:1";
    std::vector<int> descriptors = open_ssdp_sockets();
    if (descriptors.empty()) {
        error = "Unable to open an SSDP socket";
        return {};
    }

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kPort);
    (void)inet_pton(
        AF_INET, kAddress, &destination.sin_addr);
    const int bounded_wait = std::clamp(wait_ms, 500, 5000);
    const int mx = std::clamp(bounded_wait / 1000, 1, 5);
    const std::string request =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: " + std::string(kAddress) + ":" +
            std::to_string(kPort) + "\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: " + std::to_string(mx) + "\r\n"
        "ST: " + target + "\r\n\r\n";
    for (int repeat = 0; repeat < 2; ++repeat) {
        for (const int descriptor : descriptors) {
            (void)sendto(
                descriptor,
                request.data(),
                request.size(),
                0,
                reinterpret_cast<sockaddr*>(&destination),
                sizeof(destination));
        }
    }

    std::vector<pollfd> poll_descriptors;
    poll_descriptors.reserve(descriptors.size());
    for (const int descriptor : descriptors) {
        poll_descriptors.push_back(
            {descriptor, POLLIN, 0});
    }
    std::vector<SsdpCandidate> candidates;
    std::set<std::string> seen;
    const std::int64_t deadline =
        monotonic_milliseconds() + bounded_wait;
    while (!cancel.load(std::memory_order_relaxed) &&
           monotonic_milliseconds() < deadline &&
           candidates.size() < kMaxSsdpResponses) {
        const int timeout = std::min(
            remaining_timeout(deadline), 200);
        const int result = poll(
            poll_descriptors.data(),
            static_cast<nfds_t>(poll_descriptors.size()),
            timeout);
        if (result < 0 && errno != EINTR) {
            error = "SSDP discovery failed";
            break;
        }
        if (result <= 0) {
            continue;
        }
        for (pollfd& state : poll_descriptors) {
            if ((state.revents & POLLIN) == 0) {
                continue;
            }
            char buffer[8192];
            const ssize_t received =
                recv(state.fd, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                continue;
            }
            const std::string_view response(
                buffer,
                static_cast<std::size_t>(received));
            if (!response.starts_with("HTTP/1.1 200") &&
                !response.starts_with("HTTP/1.0 200")) {
                continue;
            }
            SsdpCandidate candidate;
            candidate.location =
                dlna_header_value(response, "LOCATION");
            candidate.usn =
                dlna_header_value(response, "USN");
            DlnaHttpUrl validated;
            if (candidate.location.empty() ||
                !parse_dlna_http_url(
                    candidate.location, validated)) {
                continue;
            }
            const std::string key =
                candidate.usn.empty()
                    ? candidate.location
                    : candidate.usn;
            if (seen.insert(key).second) {
                candidates.push_back(
                    std::move(candidate));
            }
        }
    }
    for (const int descriptor : descriptors) {
        close(descriptor);
    }
    return candidates;
}

} // namespace

std::vector<DlnaServer> discover_dlna_servers(
    int wait_ms,
    std::atomic<bool>& cancel,
    std::string& error) {
    error.clear();
    const std::vector<SsdpCandidate> candidates =
        search_ssdp(wait_ms, cancel, error);
    if (cancel.load(std::memory_order_relaxed)) {
        error = "Cancelled";
        return {};
    }

    std::vector<DlnaServer> servers;
    std::set<std::string> seen;
    std::string last_error;
    const std::int64_t description_deadline =
        monotonic_milliseconds() + kDescriptionBudgetMs;
    const std::size_t count =
        std::min(candidates.size(), kMaxDescribedServers);
    for (std::size_t index = 0;
         index < count &&
         !cancel.load(std::memory_order_relaxed);
         ++index) {
        const int remaining =
            remaining_timeout(description_deadline);
        if (remaining <= 0) {
            last_error =
                "DLNA device descriptions timed out";
            break;
        }
        DlnaServer server;
        std::string describe_error;
        if (!describe_server(
                candidates[index].location,
                std::min(
                    remaining,
                    kDescriptionRequestTimeoutMs),
                cancel,
                server,
                describe_error)) {
            if (!describe_error.empty() &&
                describe_error != "Cancelled") {
                last_error = std::move(describe_error);
            }
            continue;
        }
        const std::string key =
            server.udn.empty()
                ? server.control_url
                : server.udn;
        if (seen.insert(key).second) {
            servers.push_back(std::move(server));
        }
    }
    std::sort(
        servers.begin(),
        servers.end(),
        [](const DlnaServer& left, const DlnaServer& right) {
            return lower_ascii(left.friendly_name) <
                lower_ascii(right.friendly_name);
        });
    if (servers.empty() && error.empty() &&
        !last_error.empty()) {
        error = std::move(last_error);
    }
    return servers;
}

bool browse_dlna_directory(
    const DlnaServer& server,
    const std::string& object_id,
    std::size_t max_objects,
    std::atomic<bool>& cancel,
    DlnaBrowseResult& output,
    std::string& error) {
    output = {};
    error.clear();
    const std::size_t bounded_max =
        std::clamp<std::size_t>(max_objects, 1, 5000);
    std::uint32_t start = 0;
    while (!cancel.load(std::memory_order_relaxed)) {
        std::uint32_t returned = 0;
        if (!browse_page(
                server,
                object_id,
                start,
                200,
                cancel,
                output,
                returned,
                error)) {
            if (!output.objects.empty()) {
                output.truncated = true;
                return true;
            }
            return false;
        }
        if (returned == 0) {
            break;
        }
        if (UINT32_MAX - start < returned) {
            output.truncated = true;
            break;
        }
        start += returned;
        if ((output.total_matches > 0 &&
             start >= output.total_matches) ||
            output.objects.size() >= bounded_max) {
            output.truncated =
                output.total_matches > output.objects.size();
            break;
        }
    }
    if (cancel.load(std::memory_order_relaxed)) {
        error = "Cancelled";
        return false;
    }
    if (output.objects.size() > bounded_max) {
        output.objects.resize(bounded_max);
        output.truncated = true;
    }
    return true;
}

} // namespace bfplayer
