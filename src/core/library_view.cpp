#include "ps5mc/library_view.hpp"

#include "ps5mc/library_scanner.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>

namespace ps5mc {
namespace {

unsigned char lower_ascii_byte(unsigned char value) {
    return value < 0x80U
        ? static_cast<unsigned char>(std::tolower(value))
        : value;
}

bool ascii_case_insensitive_contains(std::string_view value, std::string_view token) {
    if (token.empty()) {
        return true;
    }
    return std::search(
               value.begin(),
               value.end(),
               token.begin(),
               token.end(),
               [](unsigned char left, unsigned char right) {
                   return lower_ascii_byte(left) == right;
               }) != value.end();
}

bool name_less(
    const std::vector<MediaEntry>& entries,
    std::size_t left_index,
    std::size_t right_index) {
    const MediaEntry& left = entries[left_index];
    const MediaEntry& right = entries[right_index];
    const std::string& left_name = left.title.empty() ? left.name : left.title;
    const std::string& right_name = right.title.empty() ? right.name : right.title;
    if (natural_path_less(left_name, right_name)) {
        return true;
    }
    if (natural_path_less(right_name, left_name)) {
        return false;
    }
    if (natural_path_less(left.path, right.path)) {
        return true;
    }
    if (natural_path_less(right.path, left.path)) {
        return false;
    }
    return left_index < right_index;
}

std::int64_t effective_duration(const MediaEntry& entry) {
    return entry.duration_ms > 0 ? entry.duration_ms : entry.resume_duration_ms;
}

} // namespace

const char* library_sort_mode_name(LibrarySortMode mode) noexcept {
    switch (mode) {
        case LibrarySortMode::name:
            return "NAME";
        case LibrarySortMode::recently_played:
            return "RECENTLY PLAYED";
        case LibrarySortMode::newest:
            return "NEWEST FILE";
        case LibrarySortMode::duration:
            return "DURATION";
        case LibrarySortMode::size:
            return "FILE SIZE";
        case LibrarySortMode::smart:
        default:
            return "SMART";
    }
}

const char* library_sort_mode_key(LibrarySortMode mode) noexcept {
    switch (mode) {
        case LibrarySortMode::name:
            return "name";
        case LibrarySortMode::recently_played:
            return "recent";
        case LibrarySortMode::newest:
            return "newest";
        case LibrarySortMode::duration:
            return "duration";
        case LibrarySortMode::size:
            return "size";
        case LibrarySortMode::smart:
        default:
            return "smart";
    }
}

std::optional<LibrarySortMode> parse_library_sort_mode(std::string_view value) noexcept {
    if (value == "smart") {
        return LibrarySortMode::smart;
    }
    if (value == "name") {
        return LibrarySortMode::name;
    }
    if (value == "recent") {
        return LibrarySortMode::recently_played;
    }
    if (value == "newest") {
        return LibrarySortMode::newest;
    }
    if (value == "duration") {
        return LibrarySortMode::duration;
    }
    if (value == "size") {
        return LibrarySortMode::size;
    }
    return std::nullopt;
}

LibrarySortMode next_library_sort_mode(LibrarySortMode mode) noexcept {
    switch (mode) {
        case LibrarySortMode::smart:
            return LibrarySortMode::name;
        case LibrarySortMode::name:
            return LibrarySortMode::recently_played;
        case LibrarySortMode::recently_played:
            return LibrarySortMode::newest;
        case LibrarySortMode::newest:
            return LibrarySortMode::duration;
        case LibrarySortMode::duration:
            return LibrarySortMode::size;
        case LibrarySortMode::size:
        default:
            return LibrarySortMode::smart;
    }
}

std::string normalize_media_query(std::string_view query) {
    std::string normalized;
    normalized.reserve(query.size());
    bool pending_space = false;
    for (unsigned char ch : query) {
        if (ch < 0x80U && std::isspace(ch) != 0) {
            pending_space = !normalized.empty();
            continue;
        }
        if (pending_space) {
            normalized.push_back(' ');
            pending_space = false;
        }
        normalized.push_back(static_cast<char>(lower_ascii_byte(ch)));
    }
    return normalized;
}

bool media_matches_query(const MediaEntry& entry, std::string_view query) {
    const std::string normalized = normalize_media_query(query);
    return media_matches_normalized_query(entry, normalized);
}

bool media_matches_normalized_query(
    const MediaEntry& entry,
    std::string_view normalized_query) {
    if (normalized_query.empty()) {
        return true;
    }

    std::size_t token_start = 0;
    while (token_start < normalized_query.size()) {
        const std::size_t token_end = normalized_query.find(' ', token_start);
        const std::size_t length = token_end == std::string::npos
            ? normalized_query.size() - token_start
            : token_end - token_start;
        const std::string_view token = normalized_query.substr(token_start, length);
        const bool matched =
            ascii_case_insensitive_contains(entry.path, token) ||
            ascii_case_insensitive_contains(entry.name, token) ||
            ascii_case_insensitive_contains(entry.title, token) ||
            ascii_case_insensitive_contains(entry.container, token) ||
            ascii_case_insensitive_contains(entry.video_codec, token) ||
            ascii_case_insensitive_contains(entry.audio_codec, token);
        if (!matched) {
            return false;
        }
        if (token_end == std::string::npos) {
            break;
        }
        token_start = token_end + 1;
    }
    return true;
}

void sort_media_indices(
    const std::vector<MediaEntry>& entries,
    std::vector<std::size_t>& indices,
    LibrarySortMode mode,
    bool smart_prefers_recent) {
    LibrarySortMode effective = mode;
    if (mode == LibrarySortMode::smart) {
        effective = smart_prefers_recent
            ? LibrarySortMode::recently_played
            : LibrarySortMode::name;
    }

    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t left, std::size_t right) {
        const MediaEntry& left_entry = entries[left];
        const MediaEntry& right_entry = entries[right];
        switch (effective) {
            case LibrarySortMode::recently_played:
                if (left_entry.last_played_unix != right_entry.last_played_unix) {
                    return left_entry.last_played_unix > right_entry.last_played_unix;
                }
                break;
            case LibrarySortMode::newest:
                if (left_entry.modified_unix != right_entry.modified_unix) {
                    return left_entry.modified_unix > right_entry.modified_unix;
                }
                break;
            case LibrarySortMode::duration: {
                const std::int64_t left_duration = effective_duration(left_entry);
                const std::int64_t right_duration = effective_duration(right_entry);
                if (left_duration != right_duration) {
                    return left_duration > right_duration;
                }
                break;
            }
            case LibrarySortMode::size:
                if (left_entry.size != right_entry.size) {
                    return left_entry.size > right_entry.size;
                }
                break;
            case LibrarySortMode::smart:
            case LibrarySortMode::name:
            default:
                break;
        }
        return name_less(entries, left, right);
    });
}

std::string media_season_root(
    const MediaEntry& entry,
    std::string_view series_root) {
    if (entry.path.empty() || series_root.empty()) {
        return {};
    }
    std::string normalized_path = entry.path;
    std::string normalized_root(series_root);
    std::replace(normalized_path.begin(), normalized_path.end(), '\\', '/');
    std::replace(normalized_root.begin(), normalized_root.end(), '\\', '/');
    while (normalized_root.size() > 1 && normalized_root.back() == '/') {
        normalized_root.pop_back();
    }
    if (normalized_path.size() <= normalized_root.size() ||
        normalized_path.compare(0, normalized_root.size(), normalized_root) != 0 ||
        normalized_path[normalized_root.size()] != '/') {
        return {};
    }
    const std::size_t relative_start = normalized_root.size() + 1;
    const std::size_t separator = normalized_path.find('/', relative_start);
    if (separator == std::string::npos || separator == relative_start) {
        return {};
    }
    return normalized_path.substr(0, separator);
}

bool erase_last_utf8_codepoint(std::string& text) noexcept {
    if (text.empty()) {
        return false;
    }
    std::size_t index = text.size() - 1;
    while (index > 0 &&
           (static_cast<unsigned char>(text[index]) & 0xC0U) == 0x80U) {
        --index;
    }
    text.resize(index);
    return true;
}

} // namespace ps5mc
