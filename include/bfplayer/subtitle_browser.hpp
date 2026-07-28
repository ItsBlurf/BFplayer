#pragma once

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

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] std::string subtitle_browser_parent(const std::string& path);
[[nodiscard]] SubtitleBrowserResult list_subtitle_directory(
    const std::string& requested_path);

} // namespace bfplayer
