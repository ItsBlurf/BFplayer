#include "bfplayer/source_uri.hpp"

#include <array>
#include <algorithm>
#include <cctype>

namespace bfplayer {
namespace {

std::size_t scheme_separator(std::string_view value) noexcept {
    const std::size_t separator = value.find("://");
    if (separator == std::string_view::npos || separator == 0 ||
        std::isalpha(static_cast<unsigned char>(value.front())) == 0) {
        return std::string_view::npos;
    }
    for (std::size_t index = 1; index < separator; ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (std::isalnum(character) == 0 && character != '+' &&
            character != '-' && character != '.') {
            return std::string_view::npos;
        }
    }
    return separator;
}

bool authority_has_credentials(std::string_view value, std::size_t separator) noexcept {
    const std::size_t authority_start = separator + 3;
    const std::size_t authority_end = value.find_first_of("/?#", authority_start);
    const std::size_t at = value.find('@', authority_start);
    return at != std::string_view::npos &&
        (authority_end == std::string_view::npos || at < authority_end);
}

} // namespace

bool is_network_uri(std::string_view value) noexcept {
    return scheme_separator(value) != std::string_view::npos;
}

bool is_supported_stream_uri(std::string_view value) noexcept {
    static constexpr std::array<std::string_view, 18> kSchemes{
        "ftp", "http", "https", "mmsh", "mmst", "rtmp", "rtmpe", "rtmps",
        "rtmpt", "rtmpte", "rtmpts", "rtp", "rtsp", "sctp", "srtp", "tcp",
        "udp", "udplite",
    };
    const std::size_t separator = scheme_separator(value);
    if (separator == std::string_view::npos) {
        return false;
    }
    std::string scheme(value.substr(0, separator));
    std::transform(
        scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return std::binary_search(kSchemes.begin(), kSchemes.end(), scheme);
}

bool uri_has_credentials(std::string_view value) noexcept {
    const std::size_t separator = scheme_separator(value);
    return separator != std::string_view::npos &&
        authority_has_credentials(value, separator);
}

bool uri_has_sensitive_components(std::string_view value) noexcept {
    const std::size_t separator = scheme_separator(value);
    if (separator == std::string_view::npos) {
        return false;
    }
    return uri_has_credentials(value) ||
        value.find('?', separator + 3) != std::string_view::npos ||
        value.find('#', separator + 3) != std::string_view::npos;
}

std::string redact_uri_secrets(std::string_view value) {
    const std::size_t separator = scheme_separator(value);
    if (separator == std::string_view::npos) {
        return std::string(value);
    }

    std::string redacted(value);
    if (authority_has_credentials(value, separator)) {
        const std::size_t authority_start = separator + 3;
        const std::size_t at = redacted.find('@', authority_start);
        redacted.replace(authority_start, at - authority_start, "<redacted>");
    }

    const std::size_t query = redacted.find('?', separator + 3);
    if (query != std::string::npos) {
        const std::size_t fragment = redacted.find('#', query + 1);
        redacted.replace(
            query + 1,
            (fragment == std::string::npos ? redacted.size() : fragment) - query - 1,
            "<redacted>");
    }
    const std::size_t fragment = redacted.find('#', separator + 3);
    if (fragment != std::string::npos) {
        redacted.replace(fragment + 1, std::string::npos, "<redacted>");
    }
    return redacted;
}

} // namespace bfplayer
