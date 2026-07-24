#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ps5mc {

struct VideoThumbnail {
    int width = 0;
    int height = 0;
    std::int64_t position_ms = 0;
    std::vector<std::uint8_t> rgba;
};

// Extracts a bounded preview from around the middle of a local video. It tries
// several interior positions when a candidate is mostly black. The operation
// uses SafeReadFile, a global deadline, and an optional cancellation flag.
bool extract_video_thumbnail(
    const std::string& path,
    VideoThumbnail& output,
    std::string& error,
    const std::atomic<bool>* cancelled = nullptr);

} // namespace ps5mc
