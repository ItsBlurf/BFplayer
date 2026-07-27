#include "bfplayer/artwork.hpp"

#include "bfplayer/library_scanner.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace bfplayer {
namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string filename_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string stem_of(const std::string& path) {
    std::string filename = filename_of(path);
    const std::size_t dot = filename.find_last_of('.');
    if (dot != std::string::npos) {
        filename.resize(dot);
    }
    return filename;
}

std::string extension_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size() ||
        (slash != std::string::npos && dot < slash)) {
        return {};
    }
    return ascii_lower(path.substr(dot + 1));
}

bool supported_extension(const std::string& extension) {
    return extension == "jpg" || extension == "jpeg" || extension == "png";
}

int name_rank(const std::string& media_stem, const std::string& candidate_stem) {
    if (candidate_stem == media_stem) {
        return 0;
    }
    constexpr std::array<std::string_view, 4> separators{"-", ".", "_", " "};
    for (const std::string_view separator : separators) {
        if (candidate_stem == media_stem + std::string(separator) + "poster") {
            return 1;
        }
    }
    if (candidate_stem == "poster") {
        return 2;
    }
    if (candidate_stem == "cover") {
        return 3;
    }
    if (candidate_stem == "folder") {
        return 4;
    }
    if (candidate_stem == "front") {
        return 5;
    }
    return std::numeric_limits<int>::max();
}

std::uint32_t read_be32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

std::uint16_t read_be16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8U) |
        static_cast<std::uint16_t>(data[1]));
}

bool inspect_png(
    const std::vector<std::uint8_t>& bytes,
    std::uint32_t& width,
    std::uint32_t& height) {
    constexpr std::array<std::uint8_t, 8> signature{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    if (bytes.size() < 24 ||
        !std::equal(signature.begin(), signature.end(), bytes.begin()) ||
        read_be32(bytes.data() + 8) != 13 ||
        std::memcmp(bytes.data() + 12, "IHDR", 4) != 0) {
        return false;
    }
    width = read_be32(bytes.data() + 16);
    height = read_be32(bytes.data() + 20);
    return width != 0 && height != 0;
}

bool is_jpeg_sof(std::uint8_t marker) {
    return (marker >= 0xc0 && marker <= 0xc3) ||
           (marker >= 0xc5 && marker <= 0xc7) ||
           (marker >= 0xc9 && marker <= 0xcb) ||
           (marker >= 0xcd && marker <= 0xcf);
}

bool inspect_jpeg(
    const std::vector<std::uint8_t>& bytes,
    std::uint32_t& width,
    std::uint32_t& height) {
    if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8) {
        return false;
    }
    std::size_t position = 2;
    while (position < bytes.size()) {
        while (position < bytes.size() && bytes[position] != 0xff) {
            ++position;
        }
        while (position < bytes.size() && bytes[position] == 0xff) {
            ++position;
        }
        if (position >= bytes.size()) {
            return false;
        }
        const std::uint8_t marker = bytes[position++];
        if (marker == 0xd9 || marker == 0xda) {
            return false;
        }
        if (marker == 0x00 || marker == 0x01 ||
            (marker >= 0xd0 && marker <= 0xd8)) {
            continue;
        }
        if (position + 2 > bytes.size()) {
            return false;
        }
        const std::uint16_t segment_size = read_be16(bytes.data() + position);
        if (segment_size < 2 ||
            static_cast<std::size_t>(segment_size) > bytes.size() - position) {
            return false;
        }
        if (is_jpeg_sof(marker)) {
            if (segment_size < 7) {
                return false;
            }
            height = read_be16(bytes.data() + position + 3);
            width = read_be16(bytes.data() + position + 5);
            return width != 0 && height != 0;
        }
        position += segment_size;
    }
    return false;
}

bool inspect_encoded_artwork(
    const std::vector<std::uint8_t>& bytes,
    ArtworkFormat& format,
    std::uint32_t& width,
    std::uint32_t& height) {
    if (inspect_png(bytes, width, height)) {
        format = ArtworkFormat::png;
        return true;
    }
    if (inspect_jpeg(bytes, width, height)) {
        format = ArtworkFormat::jpeg;
        return true;
    }
    return false;
}

bool valid_limits(const ArtworkLimits& limits) {
    return limits.max_file_bytes > 0 &&
           limits.max_dimension > 0 &&
           limits.max_pixels > 0;
}

bool dimensions_within_limits(
    std::uint32_t width,
    std::uint32_t height,
    const ArtworkLimits& limits) {
    if (width == 0 || height == 0 ||
        width > limits.max_dimension || height > limits.max_dimension) {
        return false;
    }
    return static_cast<std::uint64_t>(width) <= limits.max_pixels / height;
}

#if !defined(_WIN32)
bool fatal_io_errno(int value) {
    if (value == EIO || value == EBADF || value == EFAULT) {
        return true;
    }
#ifdef ESTALE
    if (value == ESTALE) {
        return true;
    }
#endif
    return false;
}

DIR* open_directory_no_follow(const std::string& path, dev_t expected_device) {
    struct stat before {};
    if (lstat(path.c_str(), &before) != 0 ||
        !S_ISDIR(before.st_mode) || S_ISLNK(before.st_mode) ||
        before.st_dev != expected_device) {
        return nullptr;
    }
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int descriptor = open(path.c_str(), flags);
    if (descriptor < 0) {
        return nullptr;
    }
    struct stat after {};
    if (fstat(descriptor, &after) != 0 ||
        !S_ISDIR(after.st_mode) ||
        after.st_dev != before.st_dev ||
        after.st_ino != before.st_ino) {
        close(descriptor);
        return nullptr;
    }
    DIR* directory = fdopendir(descriptor);
    if (!directory) {
        close(descriptor);
    }
    return directory;
}
#endif

#if defined(_WIN32)

std::filesystem::path utf8_filesystem_path(const std::string& path) {
    const std::u8string utf8(
        reinterpret_cast<const char8_t*>(path.data()),
        reinterpret_cast<const char8_t*>(path.data() + path.size()));
    return std::filesystem::path(utf8);
}

std::string utf8_path(const std::filesystem::path& path) {
    const std::wstring wide = path.native();
    if (wide.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return path.string();
    }
    std::string value(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        value.data(), required, nullptr, nullptr);
    return value;
}

bool is_reparse_point(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES ||
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

#endif

} // namespace

std::string match_local_artwork(
    const std::string& media_path,
    const std::vector<std::string>& candidates) {
    const std::string media_stem = ascii_lower(stem_of(media_path));
    int best_rank = std::numeric_limits<int>::max();
    std::string best;
    for (const std::string& candidate : candidates) {
        if (!supported_extension(extension_of(candidate))) {
            continue;
        }
        const int rank = name_rank(media_stem, ascii_lower(stem_of(candidate)));
        if (rank < best_rank ||
            (rank == best_rank && !candidate.empty() &&
             (best.empty() || natural_path_less(candidate, best)))) {
            best_rank = rank;
            best = candidate;
        }
    }
    return best_rank == std::numeric_limits<int>::max() ? std::string{} : best;
}

std::string find_local_artwork(
    const std::string& media_path,
    const ArtworkLimits& limits) {
    if (media_path.empty() || media_path.find("://") != std::string::npos ||
        !valid_limits(limits)) {
        return {};
    }
    const std::size_t slash = media_path.find_last_of("/\\");
    const std::string directory = slash == std::string::npos ? "." : media_path.substr(0, slash);
    std::vector<std::string> candidates;
#if defined(_WIN32)
    std::error_code error;
    const std::filesystem::path directory_path = utf8_filesystem_path(directory);
    std::filesystem::directory_iterator current(
        directory_path,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::directory_iterator end;
    while (!error && current != end) {
        const std::filesystem::directory_entry entry = *current;
        if (!is_reparse_point(entry.path()) && entry.is_regular_file(error) && !error) {
            const std::uintmax_t size = entry.file_size(error);
            if (!error && size > 0 && size <= limits.max_file_bytes) {
                const std::string path = utf8_path(entry.path());
                if (supported_extension(extension_of(path))) {
                    candidates.push_back(path);
                }
            }
        }
        error.clear();
        current.increment(error);
    }
#else
    struct stat media_status {};
    if (lstat(media_path.c_str(), &media_status) != 0 ||
        !S_ISREG(media_status.st_mode) || S_ISLNK(media_status.st_mode)) {
        return {};
    }
    DIR* handle = open_directory_no_follow(directory, media_status.st_dev);
    if (!handle) {
        return {};
    }
    errno = 0;
    const int directory_descriptor = dirfd(handle);
    if (directory_descriptor < 0) {
        closedir(handle);
        return {};
    }
    bool fatal_error = false;
    for (;;) {
        errno = 0;
        dirent* item = readdir(handle);
        if (!item) {
            fatal_error = fatal_io_errno(errno);
            break;
        }
        if (std::strcmp(item->d_name, ".") == 0 ||
            std::strcmp(item->d_name, "..") == 0) {
            continue;
        }
        std::string path = directory;
        if (!path.empty() && path.back() != '/') {
            path.push_back('/');
        }
        path += item->d_name;
        struct stat status {};
        if (fstatat(
                directory_descriptor,
                item->d_name,
                &status,
                AT_SYMLINK_NOFOLLOW) != 0) {
            int value = errno;
            if (!fatal_io_errno(value) &&
                lstat(path.c_str(), &status) == 0) {
                value = 0;
            }
            if (value != 0 && fatal_io_errno(value)) {
                fatal_error = true;
                break;
            }
            if (value != 0) {
                continue;
            }
        }
        if (S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode) &&
            status.st_dev == media_status.st_dev &&
            status.st_size > 0 &&
            static_cast<std::uint64_t>(status.st_size) <= limits.max_file_bytes &&
            supported_extension(extension_of(path))) {
            candidates.push_back(std::move(path));
        }
    }
    closedir(handle);
    if (fatal_error) {
        return {};
    }
#endif
    return match_local_artwork(media_path, candidates);
}

bool load_local_artwork(
    const std::string& path,
    ArtworkData& output,
    std::string& error,
    const ArtworkLimits& limits) {
    output = {};
    error.clear();
    if (path.empty() || !valid_limits(limits)) {
        error = "invalid artwork path or limits";
        return false;
    }

    std::vector<std::uint8_t> bytes;
#if defined(_WIN32)
    const std::filesystem::path filesystem_path = utf8_filesystem_path(path);
    std::error_code status_error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(filesystem_path, status_error);
    if (status_error || status.type() != std::filesystem::file_type::regular ||
        is_reparse_point(filesystem_path)) {
        error = "artwork is not a regular non-reparse file";
        return false;
    }
    const std::uintmax_t file_size = std::filesystem::file_size(filesystem_path, status_error);
    if (status_error || file_size == 0 || file_size > limits.max_file_bytes) {
        error = "artwork encoded size is outside limits";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(file_size));
    std::ifstream input(filesystem_path, std::ios::binary);
    if (!input ||
        !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
        input.peek() != std::char_traits<char>::eof()) {
        error = "artwork read failed or changed while reading";
        return false;
    }
#else
    struct stat before {};
    if (lstat(path.c_str(), &before) != 0 ||
        !S_ISREG(before.st_mode) || S_ISLNK(before.st_mode)) {
        error = "artwork is not a regular non-symlink file";
        return false;
    }
    if (before.st_size <= 0 ||
        static_cast<std::uint64_t>(before.st_size) > limits.max_file_bytes) {
        error = "artwork encoded size is outside limits";
        return false;
    }
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = open(path.c_str(), flags);
    if (descriptor < 0) {
        error = "artwork open failed: errno=" + std::to_string(errno);
        return false;
    }
    struct stat after {};
    if (fstat(descriptor, &after) != 0 ||
        !S_ISREG(after.st_mode) ||
        after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        after.st_size != before.st_size) {
        const int value = errno;
        close(descriptor);
        error = "artwork changed between lstat and open";
        if (value != 0) {
            error += ": errno=" + std::to_string(value);
        }
        return false;
    }
    bytes.resize(static_cast<std::size_t>(after.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t read_count =
            read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (read_count > 0) {
            offset += static_cast<std::size_t>(read_count);
            continue;
        }
        if (read_count < 0 && errno == EINTR) {
            continue;
        }
        const int value = read_count == 0 ? 0 : errno;
        close(descriptor);
        error = read_count == 0
            ? "artwork ended before its stat size"
            : "artwork read failed: errno=" + std::to_string(value);
        return false;
    }
    close(descriptor);
#endif

    ArtworkFormat format = ArtworkFormat::unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!inspect_encoded_artwork(bytes, format, width, height)) {
        error = "artwork is not a valid JPEG or PNG header";
        return false;
    }
    if (!dimensions_within_limits(width, height, limits)) {
        error = "artwork dimensions exceed limits";
        return false;
    }

    output.path = path;
    output.format = format;
    output.width = width;
    output.height = height;
    output.encoded = std::move(bytes);
    return true;
}

} // namespace bfplayer
