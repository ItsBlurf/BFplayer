#include "bfplayer/playlist.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    check(static_cast<bool>(output), "test playlist writes");
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string value = path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(value.data()), value.size());
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("bfplayer-playlist-tests-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);

    const fs::path m3u = root / "mixed.m3u";
    write_file(
        m3u,
        "\xEF\xBB\xBF#EXTM3U\r\n"
        "#EXTINF:10,Local\r\n"
         "Movie One.mkv\r\n"
         "https://media.test/live.m3u8?token=abc\r\n"
         "https://user:password@media.test/rejected.mkv\r\n"
         "smb://server/share/ignored.mkv\r\n"
        "/data/media/Absolute.mp4\r\n");
    const bfplayer::PlaylistLoadResult m3u_result =
        bfplayer::load_generic_playlist(utf8(m3u));
    check(m3u_result.recognized, "M3U recognized");
    check(m3u_result.error.empty(), "M3U loads without error");
    check(m3u_result.items.size() == 3,
          "M3U filters direct SMB and credential-bearing URLs");
    check(m3u_result.items[0] == utf8(root / "Movie One.mkv"),
          "relative M3U item resolves beside playlist");
    check(m3u_result.items[1] == "https://media.test/live.m3u8?token=abc",
          "signed network stream remains unchanged for FFmpeg");
    check(m3u_result.items[2] == "/data/media/Absolute.mp4",
          "absolute PS5 path remains unchanged");

    const fs::path pls = root / "radio.pls";
    write_file(
        pls,
        "[playlist]\nNumberOfEntries=2\n"
        "File1=http://radio.test/one.mp3\nTitle1=One\n"
        "File2=Album/Track.flac\nVersion=2\n");
    const bfplayer::PlaylistLoadResult pls_result =
        bfplayer::load_generic_playlist(utf8(pls));
    check(pls_result.items.size() == 2, "PLS FileN entries parsed");
    check(pls_result.items[0] == "http://radio.test/one.mp3", "PLS URL retained");
    check(pls_result.items[1] == utf8(root / "Album" / "Track.flac"),
          "PLS relative path resolved");

    const fs::path xspf = root / "video.xspf";
    write_file(
        xspf,
        "<?xml version=\"1.0\"?><playlist><trackList>"
        "<track><location>file:///data/media/Movie%20One.mkv</location></track>"
        "<track><location>https://media.test/movie.mkv?a=1&amp;b=2</location></track>"
        "</trackList></playlist>");
    const bfplayer::PlaylistLoadResult xspf_result =
        bfplayer::load_generic_playlist(utf8(xspf));
    check(xspf_result.items.size() == 2, "XSPF locations parsed");
    check(xspf_result.items[0] == "/data/media/Movie One.mkv",
          "file URI decoded to local PS5 path");
    check(xspf_result.items[1] == "https://media.test/movie.mkv?a=1&b=2",
          "XSPF XML entities decoded");

    const bfplayer::PlaylistLoadResult truncated =
        bfplayer::load_generic_playlist(utf8(m3u), 1024U * 1024U, 1);
    check(truncated.items.size() == 1 && truncated.truncated,
          "playlist item limit is enforced");
    const bfplayer::PlaylistLoadResult too_small =
        bfplayer::load_generic_playlist(utf8(m3u), 8, 10);
    check(too_small.items.empty() && !too_small.error.empty(),
          "playlist byte limit is enforced");
    check(!bfplayer::is_generic_playlist_path("live.m3u8"),
          "M3U8 remains assigned to FFmpeg HLS");

    fs::remove_all(root);
    std::cout << "playlist_tests: PASS\n";
    return 0;
}
