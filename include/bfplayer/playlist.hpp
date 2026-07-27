#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace bfplayer {

struct PlaylistLoadResult {
    bool recognized = false;
    bool truncated = false;
    std::vector<std::string> items;
    std::string error;
};

// Generic local playlists are expanded by the library. M3U8 is intentionally
// excluded because FFmpeg must see the complete HLS manifest itself.
[[nodiscard]] bool is_generic_playlist_path(const std::string& path);

// Reads one regular, non-symlink file with strict size/item limits. Relative
// entries are resolved beside the playlist; supported network URLs are kept.
[[nodiscard]] PlaylistLoadResult load_generic_playlist(
    const std::string& path,
    std::size_t max_bytes = 1024U * 1024U,
    std::size_t max_items = 2048U);

} // namespace bfplayer
