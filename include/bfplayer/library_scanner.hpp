#pragma once

#include "bfplayer/media.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace bfplayer {

struct ScanLimits {
    std::size_t max_depth = 32;
    std::size_t max_entries_seen = 250000;
    std::size_t max_media_items = 100000;
};

struct ScanResult {
    std::size_t entries_seen = 0;
    std::size_t media_items = 0;
    std::size_t skipped_symlinks = 0;
    std::size_t skipped_devices = 0;
    std::size_t recoverable_errors = 0;
    int first_recoverable_errno = 0;
    std::string first_recoverable_path;
    int fatal_errno = 0;
    std::string fatal_path;
    bool complete = true;

    [[nodiscard]] bool ok() const noexcept { return fatal_errno == 0; }
    [[nodiscard]] bool fully_enumerated() const noexcept {
        return ok() && complete;
    }
};

using MediaVisitor = std::function<bool(const MediaEntry&)>;
using CancelCheck = std::function<bool()>;

[[nodiscard]] MediaKind classify_media_path(const std::string& path);
[[nodiscard]] bool natural_path_less(const std::string& left, const std::string& right);

// Returns sidecars in natural filename order. A sidecar must share the media
// basename exactly or use a suffix separator such as '.', '-', '_', or space
// (for example: Movie.en.srt or Movie-forced.ass).
[[nodiscard]] std::vector<std::string> match_subtitle_sidecars(
    const std::string& media_path,
    const std::vector<std::string>& candidates);

// Enumerates only the media file's immediate directory with lstat semantics,
// then applies match_subtitle_sidecars().
[[nodiscard]] std::vector<std::string> find_subtitle_sidecars(
    const std::string& media_path);

// The visitor returns false to cancel without turning cancellation into an
// error. Traversal never follows symlinks and does not cross the root st_dev on
// POSIX/PS5. EIO, ESTALE, EBADF, and EFAULT abort immediately.
[[nodiscard]] ScanResult scan_media_library(
    const std::string& root,
    const MediaVisitor& visitor,
    const ScanLimits& limits = {},
    const CancelCheck& cancelled = {});

} // namespace bfplayer
