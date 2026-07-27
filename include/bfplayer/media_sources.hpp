#pragma once

#include "bfplayer/media.hpp"

#include <string>
#include <vector>

namespace bfplayer {

enum class MediaSourceKind {
    movie_file,
    tv_folder,
};

struct MediaSource {
    MediaSourceKind kind = MediaSourceKind::movie_file;
    std::string path;
    std::string title;
};

[[nodiscard]] std::string normalize_media_source_path(std::string path);
[[nodiscard]] std::string media_source_default_title(const std::string& path);
[[nodiscard]] std::string serialize_media_sources(
    const std::vector<MediaSource>& sources);
[[nodiscard]] std::vector<MediaSource> parse_media_sources(
    const std::string& encoded);
void annotate_media_sources(
    std::vector<MediaEntry>& entries,
    const std::vector<MediaSource>& sources);
[[nodiscard]] bool media_entry_is_covered_by_sources(
    const MediaEntry& entry,
    const std::vector<MediaSource>& sources);
void retain_configured_media(
    std::vector<MediaEntry>& entries,
    const std::vector<MediaSource>& sources);

} // namespace bfplayer
