#include "bfplayer/media_sources.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    using namespace bfplayer;
    const std::vector<MediaSource> sources{
        {MediaSourceKind::tv_folder, "/mnt/usb0/One Pace/", "One Pace"},
        {MediaSourceKind::movie_file, "/mnt/usb0/Movie.mkv", "Movie"},
    };
    const std::string encoded = serialize_media_sources(sources);
    const std::vector<MediaSource> decoded = parse_media_sources(encoded);
    check(decoded.size() == 2, "source round trip count");
    check(decoded[0].path == "/mnt/usb0/One Pace" &&
              decoded[0].kind == MediaSourceKind::tv_folder,
          "TV source normalization");
    check(decoded[1].title == "Movie", "movie source title");

    std::vector<MediaEntry> entries{
        {"/mnt/usb0/One Pace/Episode 1.mkv", "Episode 1.mkv", MediaKind::video},
        {"/mnt/usb0/Movie.mkv", "Movie.mkv", MediaKind::video},
    };
    annotate_media_sources(entries, decoded);
    check(entries[0].series_root == "/mnt/usb0/One Pace" &&
              entries[0].series_title == "One Pace",
          "TV source annotation");
    check(entries[1].explicit_movie && entries[1].series_root.empty(),
          "single file stays a movie");
    std::vector<MediaEntry> sibling{
        {"/mnt/usb0/One Pace Extra/Episode.mkv",
         "Episode.mkv",
         MediaKind::video},
    };
    annotate_media_sources(sibling, decoded);
    check(sibling[0].series_root.empty(), "series root uses a path boundary");
    check(!media_entry_is_covered_by_sources(sibling[0], decoded),
          "unconfigured sibling is not covered");
    sibling.push_back(entries[0]);
    retain_configured_media(sibling, decoded);
    check(sibling.size() == 1 &&
              sibling[0].path == "/mnt/usb0/One Pace/Episode 1.mkv",
          "manual source mode removes legacy auto-indexed rows");

    std::vector<MediaSource> nested_sources{
        {MediaSourceKind::movie_file,
         "/mnt/usb0/Series/Special.mkv",
         "Special"},
        {MediaSourceKind::tv_folder, "/mnt/usb0/Series", "Series"},
        {MediaSourceKind::tv_folder, "/mnt/usb0/Series/Season 2", "Season 2"},
    };
    std::vector<MediaEntry> nested_entries{
        {"/mnt/usb0/Series/Special.mkv", "Special.mkv", MediaKind::video},
        {"/mnt/usb0/Series/Season 2/S02E01.mkv",
         "S02E01.mkv",
         MediaKind::video},
    };
    annotate_media_sources(nested_entries, nested_sources);
    check(nested_entries[0].explicit_movie &&
              nested_entries[0].series_root.empty(),
          "explicit movie overrides containing TV folder");
    check(nested_entries[1].series_root == "/mnt/usb0/Series/Season 2" &&
              nested_entries[1].series_title == "Season 2",
          "most specific TV folder wins");
    check(media_source_default_title("/mnt/usb0/My_Show") == "My Show",
          "default source title");
    return 0;
}
