#include "bfplayer/subtitle_provider.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    check(
        bfplayer::normalize_subtitle_languages(" EN; ar,FA ") ==
            "en,ar,fa",
        "language list normalizes");
    check(
        bfplayer::normalize_subtitle_languages("%%%") == "en",
        "invalid language list falls back");

    const std::string url = bfplayer::subdl_search_url(
        "/mnt/usb0/Dune Part Two [2024].mkv",
        "EN,AR");
    check(
        url.find(
            "/api/v2/files/search?filename="
            "Dune%20Part%20Two%20%5B2024%5D.mkv") !=
            std::string::npos,
        "dedicated filename search is used without exposing a local path");
    check(
        url.find("languages=en%2Car") != std::string::npos,
        "languages are normalized and encoded");
    check(
        url.find("api_key") == std::string::npos,
        "API key is never placed in the URL");
    const std::string title_url = bfplayer::subdl_title_search_url(
        "One Piece S01E01",
        "en,ar");
    check(
        title_url.find(
            "/api/v2/subtitles/search?film_name="
            "One%20Piece%20S01E01") != std::string::npos,
        "manual title search uses the film-name endpoint");
    check(
        title_url.find("api_key") == std::string::npos,
        "manual title URL never contains the API key");

    const std::string fixture = R"json(
{
  "subtitles": [
    {
      "n_id": "sub_123",
      "language": "AR",
      "release_name": "Dune.Part.Two.2024.BluRay",
      "format": "srt",
      "fps": "23.976",
      "hi": false,
      "unpack_files": [
        {
          "name": "Dune.Part.Two.ar.srt",
          "language": "AR",
          "format": "srt",
          "url": "/subtitle/sub_123/file_1"
        }
      ]
    },
    {
      "n_id": "sub_456",
      "language": "EN",
      "release_name": "Dune Part Two WEB-DL",
      "format": ".ASS",
      "hi": 1
    }
  ]
}
)json";
    const bfplayer::OnlineSubtitleSearch parsed =
        bfplayer::parse_subdl_search_json(fixture);
    check(parsed.ok(), "valid SubDL response parses");
    check(parsed.subtitles.size() == 2, "single files and IDs are returned");
    if (parsed.subtitles.size() == 2) {
        check(
            parsed.subtitles[0].language == "AR" &&
                parsed.subtitles[0].download_url ==
                    "https://dl.subdl.com/subtitle/sub_123/file_1",
            "unpacked download is preserved");
        check(
            parsed.subtitles[1].id == "sub_456" &&
                parsed.subtitles[1].format == "ass" &&
                parsed.subtitles[1].hearing_impaired,
            "fallback download metadata parses");
    }

    const bfplayer::OnlineSubtitleSearch unsafe =
        bfplayer::parse_subdl_search_json(R"json(
{"subtitles":[{"n_id":"bad/id","url":"https://evil.invalid/a.srt"}]}
)json");
    check(!unsafe.ok(), "unsafe provider IDs and URLs are rejected");

    const bfplayer::OnlineSubtitleSearch provider_error =
        bfplayer::parse_subdl_search_json(
            R"json({"error":{"code":"quota","message":"Daily limit reached"}})json");
    check(
        !provider_error.ok() &&
            provider_error.error == "Daily limit reached",
        "provider errors are surfaced");

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "bfplayer-subtitle-provider-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    bfplayer::OnlineSubtitleDownload download;
    download.bytes = {'1', '\n', '0', '0', ':', '0', '0', '\n'};
    download.extension = "SRT";
    std::string saved_path;
    std::string save_error;
    check(
        bfplayer::save_downloaded_subtitle(
            root.string(),
            "/media/Dune.mkv",
            parsed.subtitles.front(),
            download,
            saved_path,
            save_error),
        "download is atomically saved");
    check(
        !saved_path.empty() &&
            std::filesystem::exists(std::filesystem::path(saved_path)),
        "saved subtitle exists");
    std::filesystem::remove_all(root, error);

    if (failures == 0) {
        std::cout << "subtitle_provider_tests: PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
