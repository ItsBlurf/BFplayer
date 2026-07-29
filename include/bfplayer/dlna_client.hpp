#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bfplayer {

struct DlnaHttpUrl {
    std::string host;
    std::uint16_t port = 80;
    std::string path = "/";
};

struct DlnaServer {
    std::string friendly_name;
    std::string model;
    std::string udn;
    std::string location;
    std::string control_url;
    std::string content_directory_type =
        "urn:schemas-upnp-org:service:ContentDirectory:1";
};

struct DlnaObject {
    std::string id;
    std::string parent_id;
    std::string title;
    std::string upnp_class;
    std::string resource_url;
    std::string protocol_info;
    std::string artwork_url;
    std::string resolution;
    std::int64_t duration_us = -1;
    std::int64_t size_bytes = -1;
    int child_count = -1;
    bool container = false;

    [[nodiscard]] bool playable() const noexcept {
        return !container && !resource_url.empty();
    }
};

struct DlnaBrowseResult {
    std::vector<DlnaObject> objects;
    std::uint32_t total_matches = 0;
    bool truncated = false;
};

[[nodiscard]] bool parse_dlna_http_url(
    std::string_view value,
    DlnaHttpUrl& output) noexcept;
[[nodiscard]] std::string format_dlna_http_authority(
    const DlnaHttpUrl& url);
[[nodiscard]] std::string resolve_dlna_url(
    std::string_view base,
    std::string_view reference);
[[nodiscard]] std::string dlna_header_value(
    std::string_view response,
    std::string_view name);
[[nodiscard]] std::int64_t parse_dlna_duration_us(
    std::string_view value) noexcept;

// These operations are blocking and must run on a worker thread.
[[nodiscard]] std::vector<DlnaServer> discover_dlna_servers(
    int wait_ms,
    std::atomic<bool>& cancel,
    std::string& error);
[[nodiscard]] bool browse_dlna_directory(
    const DlnaServer& server,
    const std::string& object_id,
    std::size_t max_objects,
    std::atomic<bool>& cancel,
    DlnaBrowseResult& output,
    std::string& error);

} // namespace bfplayer
