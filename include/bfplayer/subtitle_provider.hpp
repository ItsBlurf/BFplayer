#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bfplayer {

struct OnlineSubtitle {
    std::string id;
    std::string language;
    std::string release_name;
    std::string format;
    std::string download_url;
    std::string fps;
    bool hearing_impaired = false;
};

struct OnlineSubtitleSearch {
    std::vector<OnlineSubtitle> subtitles;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

struct OnlineSubtitleDownload {
    std::vector<std::uint8_t> bytes;
    std::string extension;
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty() && !bytes.empty();
    }
};

[[nodiscard]] std::string normalize_subtitle_languages(std::string value);
[[nodiscard]] std::string subdl_search_url(
    const std::string& media_filename,
    const std::string& languages);
[[nodiscard]] std::string subdl_title_search_url(
    const std::string& title,
    const std::string& languages);
[[nodiscard]] OnlineSubtitleSearch parse_subdl_search_json(
    const std::string& json);
[[nodiscard]] OnlineSubtitleSearch search_subdl(
    const std::string& api_key,
    const std::string& media_filename,
    const std::string& languages);
[[nodiscard]] OnlineSubtitleSearch search_subdl_title(
    const std::string& api_key,
    const std::string& title,
    const std::string& languages);
[[nodiscard]] OnlineSubtitleDownload download_subdl(
    const std::string& api_key,
    const OnlineSubtitle& subtitle);
[[nodiscard]] bool save_downloaded_subtitle(
    const std::string& root,
    const std::string& media_path,
    const OnlineSubtitle& subtitle,
    const OnlineSubtitleDownload& download,
    std::string& saved_path,
    std::string& error);

} // namespace bfplayer
