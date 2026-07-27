#pragma once

#include "bfplayer/media.hpp"

#include <string>

namespace bfplayer {

// Probes one local media item with bounded FFmpeg analysis. On success the
// metadata fields of entry are populated while its scanner fingerprint stays
// unchanged. The caller decides how failed probes are persisted.
bool probe_media_metadata(
    MediaEntry& entry,
    std::string& error,
    int* fatal_io_error = nullptr);

} // namespace bfplayer
