#include "bfplayer/subtitle_browser.hpp"

#include <algorithm>
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

void touch(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).put('\n');
}

} // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "bfplayer-subtitle-browser-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    touch(root / "Movie.en.srt");
    touch(root / "Movie.ar.ass");
    touch(root / "ignore.mkv");
    touch(root / "Season 1" / "Episode.srt");
    std::filesystem::create_directory_symlink(
        root / "Season 1",
        root / "linked",
        error);

    const bfplayer::SubtitleBrowserResult result =
        bfplayer::list_subtitle_directory(root.string());
    check(result.ok(), "subtitle directory opens");
    check(result.entries.size() == 3, "only folders and subtitle files appear");
    check(
        result.directories == 1 && result.subtitle_files == 2,
        "browser diagnostics classify visible entries");
    if (result.entries.size() == 3) {
        check(
            result.entries[0].directory &&
                result.entries[0].name == "Season 1",
            "directories sort first");
        check(
            !result.entries[1].directory &&
                result.entries[1].name == "Movie.ar.ass",
            "subtitle files use natural order");
    }
    std::string expected_parent = root.string();
    std::replace(
        expected_parent.begin(),
        expected_parent.end(),
        '\\',
        '/');
    check(
        bfplayer::subtitle_browser_parent(
            (root / "Season 1").string()) ==
            expected_parent,
        "parent navigation is stable");

    std::filesystem::remove_all(root, error);
    if (failures == 0) {
        std::cout << "subtitle_browser_tests: PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
