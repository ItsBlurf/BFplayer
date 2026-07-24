#pragma once

#include "ps5mc/media.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ps5mc {

enum class LibrarySortMode {
    smart = 0,
    name,
    recently_played,
    newest,
    duration,
    size,
};

[[nodiscard]] const char* library_sort_mode_name(LibrarySortMode mode) noexcept;
[[nodiscard]] const char* library_sort_mode_key(LibrarySortMode mode) noexcept;
[[nodiscard]] std::optional<LibrarySortMode> parse_library_sort_mode(
    std::string_view value) noexcept;
[[nodiscard]] LibrarySortMode next_library_sort_mode(LibrarySortMode mode) noexcept;

// Search is intentionally ASCII-case-insensitive while preserving UTF-8 bytes.
// Every whitespace-separated token must match title, filename, path, container,
// or codec metadata. This avoids depending on a heavyweight Unicode library in
// the PS5 payload while still supporting exact non-ASCII searches from the IME.
[[nodiscard]] std::string normalize_media_query(std::string_view query);
[[nodiscard]] bool media_matches_query(
    const MediaEntry& entry,
    std::string_view query);
[[nodiscard]] bool media_matches_normalized_query(
    const MediaEntry& entry,
    std::string_view normalized_query);

// Sorts a filtered index set without copying MediaEntry strings. Smart sort is
// recent-first for Continue/Recently Played and natural-name order elsewhere.
void sort_media_indices(
    const std::vector<MediaEntry>& entries,
    std::vector<std::size_t>& indices,
    LibrarySortMode mode,
    bool smart_prefers_recent);

// Removes exactly one UTF-8 code point from the end of text. SDL text input is
// valid UTF-8, so malformed trailing bytes are treated as one best-effort unit.
bool erase_last_utf8_codepoint(std::string& text) noexcept;

} // namespace ps5mc
