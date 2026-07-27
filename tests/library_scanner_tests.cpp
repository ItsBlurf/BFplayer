#include "bfplayer/library_scanner.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void touch(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary);
    output << "test";
}

} // namespace

int main() {
    using bfplayer::MediaKind;
    check(bfplayer::classify_media_path("Movie.MKV") == MediaKind::video, "MKV classification");
    check(bfplayer::classify_media_path("raw.AV1") == MediaKind::video, "raw AV1 classification");
    check(bfplayer::classify_media_path("capture.WTV") == MediaKind::video, "WTV classification");
    check(bfplayer::classify_media_path("classic.SMK") == MediaKind::video, "Smacker classification");
    check(bfplayer::classify_media_path("song.FLAC") == MediaKind::audio, "FLAC classification");
    check(bfplayer::classify_media_path("audiobook.M4B") == MediaKind::audio, "M4B classification");
    check(bfplayer::classify_media_path("surround.TRUEHD") == MediaKind::audio,
          "TrueHD classification");
    check(bfplayer::classify_media_path("list.m3u8") == MediaKind::playlist, "M3U8 classification");
    check(bfplayer::classify_media_path("captions.ASS") == MediaKind::subtitle, "ASS classification");
    check(bfplayer::classify_media_path("captions.SCC") == MediaKind::subtitle, "SCC classification");
    check(bfplayer::classify_media_path("readme.md") == MediaKind::unknown, "unknown classification");

    std::vector<std::string> episodes{"Episode 10.mkv", "Episode 2.mkv", "Episode 1.mkv"};
    std::sort(episodes.begin(), episodes.end(), bfplayer::natural_path_less);
    check(episodes[0] == "Episode 1.mkv", "natural sort first value");
    check(episodes[1] == "Episode 2.mkv", "natural sort numeric value");
    check(episodes[2] == "Episode 10.mkv", "natural sort two digits");

    std::vector<std::string> comparator_corpus{
        "", "A", "a", "0", "00", "1", "01", "10",
        "Episode 2", "Episode 02", "Episode 10", "épisode 2"};
    std::uint32_t random_state = 0x5a17c3e1U;
    constexpr char alphabet[] = "AaZz019.-_ ";
    for (int item = 0; item < 52; ++item) {
        std::string value;
        random_state = random_state * 1664525U + 1013904223U;
        const std::size_t length = random_state % 18U;
        for (std::size_t index = 0; index < length; ++index) {
            random_state = random_state * 1664525U + 1013904223U;
            value.push_back(alphabet[random_state % (sizeof(alphabet) - 1U)]);
        }
        comparator_corpus.push_back(std::move(value));
    }
    bool comparator_valid = true;
    for (std::size_t left = 0; left < comparator_corpus.size(); ++left) {
        if (bfplayer::natural_path_less(
                comparator_corpus[left], comparator_corpus[left])) {
            comparator_valid = false;
        }
        for (std::size_t right = 0; right < comparator_corpus.size(); ++right) {
            const bool left_right = bfplayer::natural_path_less(
                comparator_corpus[left], comparator_corpus[right]);
            const bool right_left = bfplayer::natural_path_less(
                comparator_corpus[right], comparator_corpus[left]);
            if (left_right && right_left) {
                comparator_valid = false;
            }
            for (std::size_t third = 0;
                 third < comparator_corpus.size();
                 ++third) {
                const bool right_third = bfplayer::natural_path_less(
                    comparator_corpus[right], comparator_corpus[third]);
                if (left_right && right_third &&
                    !bfplayer::natural_path_less(
                        comparator_corpus[left], comparator_corpus[third])) {
                    comparator_valid = false;
                }
            }
        }
    }
    check(comparator_valid, "natural sort obeys strict ordering properties");

    const std::vector<std::string> candidates{
        "Movie 2.srt", "Movie.en.srt", "Movie-forced.ass", "Movie.sup",
        "Movie.idx", "Movie.sub", "unrelated-notes.txt", "Other.en.srt", "Movie.jpg"};
    const auto sidecars = bfplayer::match_subtitle_sidecars("Movie.mkv", candidates);
    check(sidecars.size() == 5, "matching sidecar count with VobSub pair deduplicated");
    check(std::find(sidecars.begin(), sidecars.end(), "Movie.sub") == sidecars.end(),
          "VobSub data file excluded when matching idx exists");
    check(std::find(sidecars.begin(), sidecars.end(), "Other.en.srt") == sidecars.end(),
          "unrelated sidecar excluded");

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "bfplayer-scanner-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "Season 1");
    std::filesystem::create_directories(root / "System Volume Information");
    std::filesystem::create_directories(root / "$RECYCLE.BIN");
    touch(root / "Movie.mkv");
    touch(root / "Movie.en.srt");
    touch(root / "Movie-forced.ass");
    touch(root / "notes.txt");
    touch(root / "Season 1" / "Episode 2.mp4");
    touch(root / "Season 1" / "Theme.flac");
    touch(root / "System Volume Information" / "hidden.mkv");
    touch(root / "$RECYCLE.BIN" / "deleted.mp4");

    std::vector<bfplayer::MediaEntry> found;
    const std::u8string root_utf8 = root.u8string();
    const std::string root_string(
        reinterpret_cast<const char*>(root_utf8.data()), root_utf8.size());
    const auto scan = bfplayer::scan_media_library(
        root_string,
        [&](const bfplayer::MediaEntry& entry) {
            found.push_back(entry);
            return true;
        });
    check(scan.ok(), "scan succeeds");
    check(scan.media_items == 3,
          "scan excludes sidecars, unknown files, and storage metadata folders");
    check(found.size() == 3, "visitor count");
    check(std::all_of(found.begin(), found.end(), [](const bfplayer::MediaEntry& entry) {
              return entry.size == 4;
          }), "file sizes captured");
    const auto discovered_sidecars = bfplayer::find_subtitle_sidecars(
        root_string + "/Movie.mkv");
    check(discovered_sidecars.size() == 2, "directory sidecar discovery");

    std::vector<bfplayer::MediaEntry> limited_found;
    const auto limited_scan = bfplayer::scan_media_library(
        root_string,
        [&](const bfplayer::MediaEntry& entry) {
            limited_found.push_back(entry);
            return true;
        },
        bfplayer::ScanLimits{32, 100, 1});
    check(limited_scan.ok() && !limited_scan.fully_enumerated(),
          "item-limited scan is explicitly incomplete");
    check(limited_found.size() == 1, "item limit stops visitation");

    const auto cancelled_scan = bfplayer::scan_media_library(
        root_string,
        [](const bfplayer::MediaEntry&) { return true; },
        {},
        []() { return true; });
    check(cancelled_scan.ok() && !cancelled_scan.fully_enumerated(),
          "cancelled scan is explicitly incomplete");

    std::filesystem::remove_all(root, error);
    if (failures == 0) {
        std::cout << "library_scanner_tests: PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
