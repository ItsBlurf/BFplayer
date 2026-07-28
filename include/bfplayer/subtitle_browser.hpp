#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace bfplayer {

struct SubtitleBrowserEntry {
    std::string name;
    std::string path;
    bool directory = false;
};

struct SubtitleBrowserResult {
    std::vector<SubtitleBrowserEntry> entries;
    std::string path;
    std::string error;
    std::size_t entries_seen = 0;
    std::size_t directories = 0;
    std::size_t subtitle_files = 0;
    std::size_t stat_fallbacks = 0;
    std::size_t unreadable_entries = 0;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] std::string subtitle_browser_parent(const std::string& path);
[[nodiscard]] SubtitleBrowserResult list_subtitle_directory(
    const std::string& requested_path);

} // namespace bfplayer
