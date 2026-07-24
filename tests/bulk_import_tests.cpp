#include "ps5mc/bulk_import.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void touch(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary);
    output.put('\0');
}

} // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "ps5mc-bulk-import-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "Show A" / "Season 01", error);
    std::filesystem::create_directories(root / "Show B", error);
    std::filesystem::create_directories(root / "Not Media", error);
    touch(root / "Movie One.mkv");
    touch(root / "Movie Two.mp4");
    touch(root / "notes.txt");
    touch(root / "Show A" / "Season 01" / "S01E01.mkv");
    touch(root / "Show B" / "Episode 01.mp4");
    touch(root / "Not Media" / "readme.txt");

    const ps5mc::BulkImportResult result =
        ps5mc::discover_bulk_media_sources(root.string());
    check(result.ok(), "bulk import completes");
    check(result.loose_movies == 2, "loose videos become two movies");
    check(result.tv_folders == 2, "video-containing child folders become shows");
    check(result.sources.size() == 4, "only intended sources are returned");

    bool show_a = false;
    bool show_b = false;
    bool movie_one = false;
    bool not_media = false;
    for (const ps5mc::MediaSource& source : result.sources) {
        show_a = show_a ||
            (source.kind == ps5mc::MediaSourceKind::tv_folder &&
             source.title == "Show A");
        show_b = show_b ||
            (source.kind == ps5mc::MediaSourceKind::tv_folder &&
             source.title == "Show B");
        movie_one = movie_one ||
            (source.kind == ps5mc::MediaSourceKind::movie_file &&
             source.title == "Movie One.mkv");
        not_media = not_media || source.path.find("Not Media") != std::string::npos;
    }
    check(show_a && show_b, "show roots keep top-level library grouping");
    check(movie_one, "movie file source keeps its filename");
    check(!not_media, "folder without video is ignored");

    const ps5mc::BulkImportResult unsafe =
        ps5mc::discover_bulk_media_sources("/");
    check(!unsafe.ok(), "filesystem root cannot be bulk imported");

    std::filesystem::remove_all(root, error);
    if (failures == 0) {
        std::cout << "bulk_import_tests: PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
