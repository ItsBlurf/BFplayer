#pragma once

#include <cstdint>
#include <string>

namespace bfplayer {

enum class MediaKind {
    unknown = 0,
    video,
    audio,
    playlist,
    subtitle,
};

struct MediaEntry {
    std::string path;
    std::string name;
    MediaKind kind = MediaKind::unknown;
    std::uint64_t size = 0;
    std::int64_t modified_unix = 0;
    std::int64_t duration_ms = 0;
    int width = 0;
    int height = 0;
    std::string container;
    std::string video_codec;
    std::string audio_codec;
    std::string title;
    std::int64_t resume_position_ms = 0;
    std::int64_t resume_duration_ms = 0;
    std::int64_t last_played_unix = 0;
    bool completed = false;
    bool favorite = false;
    // Runtime library-source annotations. They are derived from the persisted
    // source list and intentionally are not stored in the media cache.
    std::string series_root;
    std::string series_title;
    bool explicit_movie = false;
};

} // namespace bfplayer
