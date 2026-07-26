#pragma once

#include "ps5mc/media_sources.hpp"

#include <cstddef>
#include <functional>
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
    std::size_t stat_fallbacks = 0;
    std::size_t unreadable_entries = 0;
    bool cancelled = false;
    int fatal_errno = 0;
    std::string fatal_path;

    [[nodiscard]] bool ok() const noexcept { return fatal_errno == 0; }
};

struct BulkImportProgress {
    std::size_t direct_entries_checked = 0;
    std::size_t entries_checked = 0;
    std::size_t loose_movies = 0;
    std::size_t tv_folders = 0;
    std::string current_path;
};

using BulkImportCancelCheck = std::function<bool()>;
using BulkImportProgressVisitor =
    std::function<void(const BulkImportProgress&)>;

// Imports one deliberately selected library folder:
// - supported video files directly inside it become individual Movies;
// - each immediate child folder containing at least one video becomes one TV
//   Show source, with episodes discovered recursively by the normal scanner.
//
// The console filesystem implementation never follows symlinks and never
// crosses the selected root's st_dev. Descriptor-relative stat failures on
// PS5 removable storage fall back to lstat without following symlinks.
[[nodiscard]] BulkImportResult discover_bulk_media_sources(
    const std::string& selected_root,
    const BulkImportCancelCheck& cancelled = {},
    const BulkImportProgressVisitor& progress = {});

} // namespace ps5mc
