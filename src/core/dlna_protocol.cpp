#include "bfplayer/dlna_client.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace bfplayer {
namespace {

bool ascii_equal_case_insensitive(
    std::string_view left,
    std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const unsigned char l = static_cast<unsigned char>(left[index]);
        const unsigned char r = static_cast<unsigned char>(right[index]);
        if (std::tolower(l) != std::tolower(r)) {
            return false;
        }
    }
    return true;
}

bool ascii_starts_with_case_insensitive(
    std::string_view value,
    std::string_view prefix) noexcept {
    return value.size() >= prefix.size() &&
        ascii_equal_case_insensitive(
            value.substr(0, prefix.size()), prefix);
}

bool parse_decimal_port(
    std::string_view value,
    std::uint16_t& output) noexcept {
    if (value.empty()) {
        return false;
    }
    unsigned int parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() ||
        parsed == 0 || parsed > 65535) {
        return false;
    }
    output = static_cast<std::uint16_t>(parsed);
    return true;
}

} // namespace

bool parse_dlna_http_url(
    std::string_view value,
    DlnaHttpUrl& output) noexcept {
    constexpr std::string_view kScheme = "http://";
    if (value.size() < kScheme.size() ||
        !ascii_equal_case_insensitive(value.substr(0, kScheme.size()), kScheme)) {
        return false;
    }

    const std::size_t authority_start = kScheme.size();
    std::size_t authority_end =
        value.find_first_of("/?#", authority_start);
    if (authority_end == std::string_view::npos) {
        authority_end = value.size();
    }
    const std::string_view authority =
        value.substr(authority_start, authority_end - authority_start);
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return false;
    }

    DlnaHttpUrl parsed;
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos || close == 1) {
            return false;
        }
        parsed.host.assign(authority.substr(1, close - 1));
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':' ||
                !parse_decimal_port(
                    authority.substr(close + 2), parsed.port)) {
                return false;
            }
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon || colon == 0 ||
                !parse_decimal_port(
                    authority.substr(colon + 1), parsed.port)) {
                return false;
            }
            parsed.host.assign(authority.substr(0, colon));
        } else {
            parsed.host.assign(authority);
        }
    }
    if (parsed.host.empty()) {
        return false;
    }

    if (authority_end < value.size()) {
        if (value[authority_end] == '#') {
            parsed.path = "/";
        } else if (value[authority_end] == '?') {
            parsed.path = "/" + std::string(value.substr(authority_end));
        } else {
            const std::size_t fragment = value.find('#', authority_end);
            parsed.path.assign(value.substr(
                authority_end,
                fragment == std::string_view::npos
                    ? std::string_view::npos
                    : fragment - authority_end));
        }
    }
    if (parsed.path.empty()) {
        parsed.path = "/";
    }
    output = std::move(parsed);
    return true;
}

std::string resolve_dlna_url(
    std::string_view base,
    std::string_view reference) {
    if (reference.empty()) {
        return std::string(base);
    }
    if (ascii_starts_with_case_insensitive(
            reference, "http://") ||
        ascii_starts_with_case_insensitive(
            reference, "https://")) {
        return std::string(reference);
    }

    DlnaHttpUrl parsed;
    if (!parse_dlna_http_url(base, parsed)) {
        return std::string(reference);
    }
    const bool ipv6 = parsed.host.find(':') != std::string::npos;
    std::string origin = "http://";
    if (ipv6) {
        origin += "[" + parsed.host + "]";
    } else {
        origin += parsed.host;
    }
    if (parsed.port != 80) {
        origin += ":" + std::to_string(parsed.port);
    }
    if (reference.front() == '/') {
        return origin + std::string(reference);
    }

    std::string directory = parsed.path;
    const std::size_t query = directory.find('?');
    if (query != std::string::npos) {
        directory.resize(query);
    }
    const std::size_t slash = directory.rfind('/');
    directory =
        slash == std::string::npos ? "/" : directory.substr(0, slash + 1);
    return origin + directory + std::string(reference);
}

std::string dlna_header_value(
    std::string_view response,
    std::string_view name) {
    std::size_t position = 0;
    while (position < response.size()) {
        std::size_t end = response.find("\r\n", position);
        if (end == std::string_view::npos) {
            end = response.size();
        }
        const std::string_view line =
            response.substr(position, end - position);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos &&
            ascii_equal_case_insensitive(line.substr(0, colon), name)) {
            std::size_t value_start = colon + 1;
            while (value_start < line.size() &&
                   (line[value_start] == ' ' || line[value_start] == '\t')) {
                ++value_start;
            }
            std::size_t value_end = line.size();
            while (value_end > value_start &&
                   (line[value_end - 1] == ' ' ||
                    line[value_end - 1] == '\t')) {
                --value_end;
            }
            return std::string(
                line.substr(value_start, value_end - value_start));
        }
        position = end == response.size() ? end : end + 2;
    }
    return {};
}

std::int64_t parse_dlna_duration_us(
    std::string_view value) noexcept {
    const std::size_t first_colon = value.find(':');
    const std::size_t second_colon =
        first_colon == std::string_view::npos
            ? std::string_view::npos
            : value.find(':', first_colon + 1);
    if (first_colon == std::string_view::npos ||
        second_colon == std::string_view::npos ||
        value.find(':', second_colon + 1) != std::string_view::npos) {
        return -1;
    }

    std::uint64_t hours = 0;
    unsigned int minutes = 0;
    const auto hours_result = std::from_chars(
        value.data(), value.data() + first_colon, hours);
    const auto minutes_result = std::from_chars(
        value.data() + first_colon + 1,
        value.data() + second_colon,
        minutes);
    if (hours_result.ec != std::errc{} ||
        hours_result.ptr != value.data() + first_colon ||
        minutes_result.ec != std::errc{} ||
        minutes_result.ptr != value.data() + second_colon ||
        minutes > 59) {
        return -1;
    }

    const std::string seconds_text(value.substr(second_colon + 1));
    char* end = nullptr;
    const double seconds = std::strtod(seconds_text.c_str(), &end);
    if (!end || end != seconds_text.c_str() + seconds_text.size() ||
        !std::isfinite(seconds) || seconds < 0.0 || seconds >= 60.0) {
        return -1;
    }
    constexpr std::uint64_t kMicrosPerHour = 3600ULL * 1000000ULL;
    if (hours >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) /
            kMicrosPerHour) {
        return -1;
    }
    const long double total =
        static_cast<long double>(hours) * 3600.0L +
        static_cast<long double>(minutes) * 60.0L +
        static_cast<long double>(seconds);
    if (total >
        static_cast<long double>(
            std::numeric_limits<std::int64_t>::max()) /
            1000000.0L) {
        return -1;
    }
    return static_cast<std::int64_t>(total * 1000000.0L);
}

} // namespace bfplayer
