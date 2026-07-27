#include "bfplayer/artwork.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void write_bytes(
    const std::filesystem::path& path,
    const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

std::vector<unsigned char> png_header(unsigned width, unsigned height) {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R',
        static_cast<unsigned char>((width >> 24U) & 0xffU),
        static_cast<unsigned char>((width >> 16U) & 0xffU),
        static_cast<unsigned char>((width >> 8U) & 0xffU),
        static_cast<unsigned char>(width & 0xffU),
        static_cast<unsigned char>((height >> 24U) & 0xffU),
        static_cast<unsigned char>((height >> 16U) & 0xffU),
        static_cast<unsigned char>((height >> 8U) & 0xffU),
        static_cast<unsigned char>(height & 0xffU),
    };
}

std::vector<unsigned char> jpeg_header(unsigned width, unsigned height) {
    return {
        0xff, 0xd8,
        0xff, 0xe0, 0x00, 0x04, 0x00, 0x00,
        0xff, 0xc0, 0x00, 0x0b, 0x08,
        static_cast<unsigned char>((height >> 8U) & 0xffU),
        static_cast<unsigned char>(height & 0xffU),
        static_cast<unsigned char>((width >> 8U) & 0xffU),
        static_cast<unsigned char>(width & 0xffU),
        0x01, 0x01, 0x11, 0x00,
    };
}

std::string utf8_path(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(value.data()),
        value.size());
}

} // namespace

int main() {
    const std::vector<std::string> candidates{
        "/media/folder.jpg",
        "/media/cover.png",
        "/media/poster.jpeg",
        "/media/Movie-poster.jpg",
        "/media/Movie.png",
        "/media/Movie.webp",
        "/media/Other.jpg",
    };
    check(
        bfplayer::match_local_artwork("/media/Movie.mkv", candidates) == "/media/Movie.png",
        "exact title artwork wins");

    const std::vector<std::string> generic{
        "/media/front.jpg", "/media/folder.png", "/media/cover.jpg", "/media/poster.png"};
    check(
        bfplayer::match_local_artwork("/media/Movie.mkv", generic) == "/media/poster.png",
        "generic poster priority");
    check(
        bfplayer::match_local_artwork("Movie.mkv", {"Movie.webp"}).empty(),
        "unsupported artwork extension ignored");
    check(
        bfplayer::find_local_artwork("https://example.invalid/movie.mkv").empty(),
        "network sources do not scan local sidecars");

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "bfplayer-artwork-test";
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);
    std::filesystem::create_directories(root);
    write_bytes(root / "Movie.mkv", {0x00});
    write_bytes(root / "folder.jpg", jpeg_header(640, 960));
    write_bytes(root / "Movie.png", png_header(1200, 1800));
    write_bytes(root / "Movie.webp", {0x00, 0x01});

    const std::string media_path = utf8_path(root / "Movie.mkv");
    const std::string artwork_path = bfplayer::find_local_artwork(media_path);
    check(
        artwork_path == utf8_path(root / "Movie.png"),
        "immediate-directory discovery and ranking");

    bfplayer::ArtworkData artwork;
    std::string error;
    check(
        bfplayer::load_local_artwork(artwork_path, artwork, error),
        "bounded PNG header load");
    check(
        artwork.format == bfplayer::ArtworkFormat::png &&
        artwork.width == 1200 && artwork.height == 1800,
        "PNG dimensions");

    const std::string jpeg_path = utf8_path(root / "folder.jpg");
    check(
        bfplayer::load_local_artwork(jpeg_path, artwork, error),
        "bounded JPEG header load");
    check(
        artwork.format == bfplayer::ArtworkFormat::jpeg &&
        artwork.width == 640 && artwork.height == 960,
        "JPEG dimensions");

    write_bytes(root / "huge.png", png_header(9000, 9000));
    check(
        !bfplayer::load_local_artwork(utf8_path(root / "huge.png"), artwork, error),
        "oversized decoded dimensions rejected");

    bfplayer::ArtworkLimits tiny_limits;
    tiny_limits.max_file_bytes = 8;
    check(
        !bfplayer::load_local_artwork(jpeg_path, artwork, error, tiny_limits),
        "oversized encoded file rejected");

    std::filesystem::remove_all(root, filesystem_error);
    if (failures == 0) {
        std::cout << "artwork_tests: PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
