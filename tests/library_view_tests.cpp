#include "ps5mc/library_view.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    using ps5mc::LibrarySortMode;
    using ps5mc::MediaEntry;
    using ps5mc::MediaKind;

    MediaEntry episode_10{"/media/Season 1/Episode 10.mkv", "Episode 10.mkv", MediaKind::video};
    episode_10.title = "The Return";
    episode_10.container = "matroska,webm";
    episode_10.video_codec = "h264";
    episode_10.audio_codec = "aac";
    episode_10.modified_unix = 100;
    episode_10.duration_ms = 3600000;
    episode_10.size = 1000;
    episode_10.last_played_unix = 50;

    MediaEntry episode_2{"/media/Season 1/Episode 2.mkv", "Episode 2.mkv", MediaKind::video};
    episode_2.modified_unix = 200;
    episode_2.duration_ms = 1800000;
    episode_2.size = 2000;
    episode_2.last_played_unix = 75;

    check(ps5mc::media_matches_query(episode_10, "RETURN h264"),
          "query matches title and codec case-insensitively");
    check(ps5mc::media_matches_query(episode_10, "season   aac"),
          "query whitespace collapses and tokens match separate fields");
    check(!ps5mc::media_matches_query(episode_10, "return hevc"),
          "every query token is required");
    episode_10.title = "Am\xC3\xA9lie";
    check(ps5mc::media_matches_query(episode_10, "am\xC3\xA9lie"),
          "UTF-8 bytes match while ASCII letters remain case-insensitive");
    episode_10.title = "The Return";
    check(ps5mc::normalize_media_query("  MOVIE\t  Night  ") == "movie night",
          "query normalization trims and collapses ASCII whitespace");

    const std::vector<MediaEntry> entries{episode_10, episode_2};
    std::vector<std::size_t> indices{0, 1};
    ps5mc::sort_media_indices(entries, indices, LibrarySortMode::name, false);
    check(indices == std::vector<std::size_t>({1, 0}), "natural display-name sort");

    indices = {0, 1};
    ps5mc::sort_media_indices(entries, indices, LibrarySortMode::smart, true);
    check(indices == std::vector<std::size_t>({1, 0}), "smart recent-first sort");

    indices = {0, 1};
    ps5mc::sort_media_indices(entries, indices, LibrarySortMode::duration, false);
    check(indices == std::vector<std::size_t>({0, 1}), "duration descending sort");

    indices = {0, 1};
    ps5mc::sort_media_indices(entries, indices, LibrarySortMode::newest, false);
    check(indices == std::vector<std::size_t>({1, 0}), "modified-time descending sort");

    check(ps5mc::parse_library_sort_mode("size") == LibrarySortMode::size,
          "sort key parses");
    check(!ps5mc::parse_library_sort_mode("invalid").has_value(),
          "invalid sort key rejected");
    check(ps5mc::next_library_sort_mode(LibrarySortMode::size) == LibrarySortMode::smart,
          "sort cycle wraps");

    std::string utf8 = "caf\xC3\xA9";
    check(ps5mc::erase_last_utf8_codepoint(utf8) && utf8 == "caf",
          "UTF-8 backspace removes one complete code point");
    check(ps5mc::erase_last_utf8_codepoint(utf8) && utf8 == "ca",
          "ASCII backspace removes one byte");

    if (failures == 0) {
        std::cout << "library_view_tests: PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
