#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bfplayer {

enum class ArtworkFormat {
    unknown = 0,
    jpeg,
    png,
};

struct ArtworkLimits {
    std::size_t max_file_bytes = 16U * 1024U * 1024U;
    std::uint32_t max_dimension = 8192;
    std::uint64_t max_pixels = 20U * 1000U * 1000U;
};

struct ArtworkData {
    std::string path;
    ArtworkFormat format = ArtworkFormat::unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> encoded;
};

// Returns the best matching JPEG/PNG candidate. Priority is title-specific
// artwork, title-specific "-poster" artwork, then poster/cover/folder/front.
[[nodiscard]] std::string match_local_artwork(
    const std::string& media_path,
    const std::vector<std::string>& candidates);

// Enumerates only the media file's immediate directory. Symlinks/reparse
// points and oversized files are ignored.
[[nodiscard]] std::string find_local_artwork(
    const std::string& media_path,
    const ArtworkLimits& limits = {});

// Reads one regular non-symlink file once, validates JPEG/PNG dimensions before
// the UI decoder sees it, and enforces bounded encoded/decoded sizes.
[[nodiscard]] bool load_local_artwork(
    const std::string& path,
    ArtworkData& output,
    std::string& error,
    const ArtworkLimits& limits = {});

} // namespace bfplayer
