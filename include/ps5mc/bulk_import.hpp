#pragma once

#include "ps5mc/media_sources.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace ps5mc {

struct BulkImportResult {
    std::vector<MediaSource> sources;
    std::size_t loose_movies = 0;
    std::size_t tv_folders = 0;
    std::size_t entries_checked = 0;
    std::size_t skipped_symlinks = 0;
    std::size_t skipped_devices = 0;
    int fatal_errno = 0;
    std::string fatal_path;

    [[nodiscard]] bool ok() const noexcept { return fatal_errno == 0; }
};

// Imports one deliberately selected library folder:
// - supported video files directly inside it become individual Movies;
// - each immediate child folder containing at least one video becomes one TV
//   Show source, with episodes discovered recursively by the normal scanner.
//
// The console filesystem implementation never follows symlinks and never
// crosses the selected root's st_dev.
[[nodiscard]] BulkImportResult discover_bulk_media_sources(
    const std::string& selected_root);

} // namespace ps5mc
