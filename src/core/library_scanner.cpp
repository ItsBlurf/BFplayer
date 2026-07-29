#include "bfplayer/library_scanner.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <filesystem>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace bfplayer {
namespace {

using ExtensionList = std::vector<std::string_view>;

const ExtensionList kVideoExtensions{
    "264", "265", "3g2", "3gp", "anm", "apng", "asf", "av1", "avi", "bfi",
    "bik", "bink", "bmv", "c93", "cavs", "cine", "dirac", "divx", "dnxhd",
    "dv", "dxa", "evc", "f4v", "flc", "fli", "flic", "flv", "gif", "gxf",
    "h261", "h263", "h264", "h265", "hevc", "ivf", "j2k", "jv", "m2ts",
    "m2v", "m4v", "mjpeg", "mjpg", "mkv", "mlv", "mov", "mp2v", "mp4",
    "mpe", "mpeg", "mpegts", "mpg", "mts", "mtv", "mvi", "mxf", "nsv",
    "nut", "nuv", "obu", "ogm", "ogv", "pmp", "r3d", "rm", "rmvb",
    "roq", "smacker", "smk", "swf", "thp", "tmv", "ts", "vc1", "viv", "vivo",
    "vob", "vvc", "webm", "wmv", "wtv", "xmv", "y4m", "yop", "yuv"};

const ExtensionList kAudioExtensions{
    "aac", "aax", "ac3", "adp", "adx", "afc", "aif", "aiff", "aix", "alac",
    "amr", "ape", "aptx", "au", "avr", "bonk", "caf", "dff", "dsf", "dss",
    "dst", "dts", "dtshd", "eac3", "flac", "fsb", "g722", "g723", "g726",
    "g729", "genh", "gsm", "hca", "hcom", "ircam", "loas", "m4a", "m4b", "mka",
    "mlp", "mp2", "mp3", "mpc", "mpc8", "oga", "ogg", "oma", "opus", "pcm",
    "qcp", "qoa", "ra", "rka", "sbc", "shn", "sln", "sox", "spx", "tak",
    "truehd", "tta", "voc", "vqf", "w64", "wav", "weba", "wma", "wv", "xma",
    "xwma"};

const ExtensionList kPlaylistExtensions{"m3u", "m3u8", "pls", "xspf"};

const ExtensionList kSubtitleExtensions{
    "aqt", "ass", "dfxp", "idx", "jss", "lrc", "mcc", "mpl", "mpl2", "pjs",
    "rt", "sami", "scc", "smi", "smil", "srt", "ssa", "stl", "sub",
    "sup", "ttml", "txt", "vtt"};

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool skip_storage_metadata_directory(const std::string& name) {
    const std::string folded = ascii_lower(name);
    return folded == "$recycle.bin" ||
           folded == "system volume information" ||
           folded == ".spotlight-v100" ||
           folded == ".trashes" ||
           folded == "lost+found";
}

std::string extension_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) ||
        dot + 1 >= path.size()) {
        return {};
    }
    return ascii_lower(path.substr(dot + 1));
}

bool contains_extension(const ExtensionList& list, const std::string& extension) {
    assert(std::is_sorted(list.begin(), list.end()));
    return std::binary_search(list.begin(), list.end(), std::string_view(extension));
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

bool stat_directory_entry(
    int directory_descriptor,
    const std::string& full_path,
    const char* name,
    struct stat& status,
    int& error) {
    if (fstatat(
            directory_descriptor,
            name,
            &status,
            AT_SYMLINK_NOFOLLOW) == 0) {
        error = 0;
        return true;
    }
    error = errno;
    // The PS5 exFAT layer can enumerate a USB directory while rejecting
    // descriptor-relative stat with several firmware-dependent nonfatal
    // errors. lstat preserves the required no-follow semantics and is the
    // stable PS5 traversal primitive.
    if (!fatal_io_errno(error) &&
        lstat(full_path.c_str(), &status) == 0) {
        error = 0;
        return true;
    }
    if (!fatal_io_errno(error)) {
        error = errno;
    }
    return false;
}

DIR* open_directory_no_follow(
    const std::string& path,
    dev_t expected_device,
    int& error,
    bool& different_device) {
    error = 0;
    different_device = false;
    struct stat before {};
    if (lstat(path.c_str(), &before) != 0) {
        error = errno;
        return nullptr;
    }
    if (!S_ISDIR(before.st_mode) || S_ISLNK(before.st_mode)) {
        error = ENOTDIR;
        return nullptr;
    }
    if (before.st_dev != expected_device) {
        different_device = true;
        error = EXDEV;
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
        error = errno;
        return nullptr;
    }
    struct stat after {};
    if (fstat(descriptor, &after) != 0 ||
        !S_ISDIR(after.st_mode) ||
        after.st_dev != before.st_dev ||
        after.st_ino != before.st_ino) {
        error = errno;
        if (error == 0) {
#ifdef ESTALE
            error = ESTALE;
#else
            error = EIO;
#endif
        }
        close(descriptor);
        return nullptr;
    }
    DIR* directory = fdopendir(descriptor);
    if (!directory) {
        error = errno;
        close(descriptor);
        return nullptr;
    }
    return directory;
}
#endif

bool reached_limit(const ScanResult& result, const ScanLimits& limits) {
    return result.entries_seen >= limits.max_entries_seen ||
           result.media_items >= limits.max_media_items;
}

#if !defined(_WIN32)
void record_recoverable(
    ScanResult& result,
    const std::string& path,
    int error) {
    ++result.recoverable_errors;
    result.complete = false;
    if (result.first_recoverable_errno == 0) {
        result.first_recoverable_errno = error != 0 ? error : EIO;
        result.first_recoverable_path = path;
    }
}
#endif

#if defined(_WIN32)

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
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::int64_t modified_unix(const std::filesystem::file_time_type& value) {
    using namespace std::chrono;
    const auto system_value = time_point_cast<system_clock::duration>(
        value - std::filesystem::file_time_type::clock::now() + system_clock::now());
    return duration_cast<seconds>(system_value.time_since_epoch()).count();
}

#endif

} // namespace

MediaKind classify_media_path(const std::string& path) {
    const std::string extension = extension_of(path);
    if (contains_extension(kVideoExtensions, extension)) {
        return MediaKind::video;
    }
    if (contains_extension(kAudioExtensions, extension)) {
        return MediaKind::audio;
    }
    if (contains_extension(kPlaylistExtensions, extension)) {
        return MediaKind::playlist;
    }
    if (contains_extension(kSubtitleExtensions, extension)) {
        return MediaKind::subtitle;
    }
    return MediaKind::unknown;
}

bool natural_path_less(const std::string& left, const std::string& right) {
    std::size_t a = 0;
    std::size_t b = 0;
    while (a < left.size() && b < right.size()) {
        const unsigned char left_ch = static_cast<unsigned char>(left[a]);
        const unsigned char right_ch = static_cast<unsigned char>(right[b]);
        if (std::isdigit(left_ch) && std::isdigit(right_ch)) {
            std::size_t left_zero = a;
            std::size_t right_zero = b;
            while (left_zero < left.size() && left[left_zero] == '0') {
                ++left_zero;
            }
            while (right_zero < right.size() && right[right_zero] == '0') {
                ++right_zero;
            }
            std::size_t left_end = left_zero;
            std::size_t right_end = right_zero;
            while (left_end < left.size() &&
                   std::isdigit(static_cast<unsigned char>(left[left_end]))) {
                ++left_end;
            }
            while (right_end < right.size() &&
                   std::isdigit(static_cast<unsigned char>(right[right_end]))) {
                ++right_end;
            }
            const std::size_t left_digits = left_end - left_zero;
            const std::size_t right_digits = right_end - right_zero;
            if (left_digits != right_digits) {
                return left_digits < right_digits;
            }
            const int number_compare = left.compare(left_zero, left_digits, right, right_zero, right_digits);
            if (number_compare != 0) {
                return number_compare < 0;
            }
            a = left_end;
            b = right_end;
            continue;
        }

        const unsigned char folded_left = static_cast<unsigned char>(std::tolower(left_ch));
        const unsigned char folded_right = static_cast<unsigned char>(std::tolower(right_ch));
        if (folded_left != folded_right) {
            return folded_left < folded_right;
        }
        ++a;
        ++b;
    }
    return left.size() < right.size();
}

std::vector<std::string> match_subtitle_sidecars(
    const std::string& media_path,
    const std::vector<std::string>& candidates) {
    const std::string media_stem = ascii_lower(stem_of(media_path));
    std::vector<std::string> vobsub_stems;
    for (const std::string& candidate : candidates) {
        if (extension_of(candidate) == "idx") {
            vobsub_stems.push_back(ascii_lower(candidate.substr(0, candidate.find_last_of('.'))));
        }
    }
    std::vector<std::string> matches;
    for (const std::string& candidate : candidates) {
        if (classify_media_path(candidate) != MediaKind::subtitle) {
            continue;
        }
        if (extension_of(candidate) == "sub") {
            const std::string without_extension =
                ascii_lower(candidate.substr(0, candidate.find_last_of('.')));
            if (std::find(vobsub_stems.begin(), vobsub_stems.end(), without_extension) !=
                vobsub_stems.end()) {
                continue; // The .idx opens and describes its paired .sub data.
            }
        }
        const std::string subtitle_stem = ascii_lower(stem_of(candidate));
        if (subtitle_stem == media_stem) {
            matches.push_back(candidate);
            continue;
        }
        if (subtitle_stem.size() <= media_stem.size() ||
            subtitle_stem.compare(0, media_stem.size(), media_stem) != 0) {
            continue;
        }
        const char separator = subtitle_stem[media_stem.size()];
        if (separator == '.' || separator == '-' || separator == '_' || separator == ' ') {
            matches.push_back(candidate);
        }
    }
    std::sort(matches.begin(), matches.end(), natural_path_less);
    return matches;
}

std::vector<std::string> find_subtitle_sidecars(const std::string& media_path) {
    const std::size_t slash = media_path.find_last_of("/\\");
    const std::string directory = slash == std::string::npos ? "." : media_path.substr(0, slash);
    std::vector<std::string> candidates;
#if defined(_WIN32)
    std::error_code error;
    const std::u8string directory_utf8(
        reinterpret_cast<const char8_t*>(directory.data()),
        reinterpret_cast<const char8_t*>(directory.data() + directory.size()));
    std::filesystem::directory_iterator current(
        std::filesystem::path(directory_utf8),
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::directory_iterator end;
    while (!error && current != end) {
        const auto& entry = *current;
        if (!is_reparse_point(entry.path()) && entry.is_regular_file(error) && !error) {
            const std::string path = utf8_path(entry.path());
            if (classify_media_path(path) == MediaKind::subtitle) {
                candidates.push_back(path);
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
    int directory_error = 0;
    bool different_device = false;
    DIR* handle = open_directory_no_follow(
        directory,
        media_status.st_dev,
        directory_error,
        different_device);
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
        int status_error = 0;
        if (!stat_directory_entry(
                directory_descriptor,
                path,
                item->d_name,
                status,
                status_error)) {
            if (fatal_io_errno(status_error)) {
                fatal_error = true;
                break;
            }
            continue;
        }
        if (S_ISREG(status.st_mode) &&
            status.st_dev == media_status.st_dev &&
            classify_media_path(path) == MediaKind::subtitle) {
            candidates.push_back(std::move(path));
        }
    }
    closedir(handle);
    if (fatal_error) {
        return {};
    }
#endif
    return match_subtitle_sidecars(media_path, candidates);
}

ScanResult scan_media_library(
    const std::string& root,
    const MediaVisitor& visitor,
    const ScanLimits& limits,
    const CancelCheck& cancelled) {
    ScanResult result{};
    if (root.empty() || !visitor || limits.max_depth == 0 ||
        limits.max_entries_seen == 0 || limits.max_media_items == 0) {
        result.fatal_errno = EINVAL;
        result.fatal_path = root;
        return result;
    }

#if defined(_WIN32)
    std::error_code error;
    const std::u8string root_utf8(
        reinterpret_cast<const char8_t*>(root.data()),
        reinterpret_cast<const char8_t*>(root.data() + root.size()));
    const std::filesystem::path root_path(root_utf8);
    if (!std::filesystem::is_directory(root_path, error) || error) {
        result.fatal_errno = error ? static_cast<int>(error.value()) : ENOTDIR;
        result.fatal_path = root;
        return result;
    }

    std::filesystem::recursive_directory_iterator current(
        root_path,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        result.fatal_errno = static_cast<int>(error.value());
        result.fatal_path = root;
        return result;
    }

    while (current != end && !reached_limit(result, limits) &&
           (!cancelled || !cancelled())) {
        const std::filesystem::directory_entry entry = *current;
        const std::string path = utf8_path(entry.path());
        ++result.entries_seen;

        if (current.depth() >= static_cast<int>(limits.max_depth)) {
            current.disable_recursion_pending();
        }
        if (is_reparse_point(entry.path())) {
            current.disable_recursion_pending();
            ++result.skipped_symlinks;
            current.increment(error);
            if (error) {
                ++result.recoverable_errors;
                result.complete = false;
                error.clear();
            }
            continue;
        }
        if (entry.is_directory(error) && !error &&
            skip_storage_metadata_directory(
                utf8_path(entry.path().filename()))) {
            current.disable_recursion_pending();
            current.increment(error);
            if (error) {
                ++result.recoverable_errors;
                result.complete = false;
                error.clear();
            }
            continue;
        }

        if (entry.is_regular_file(error) && !error) {
            const MediaKind kind = classify_media_path(path);
            if (kind != MediaKind::unknown && kind != MediaKind::subtitle) {
                MediaEntry media{};
                media.path = path;
                media.name = filename_of(path);
                media.kind = kind;
                media.size = entry.file_size(error);
                if (error) {
                    media.size = 0;
                    ++result.recoverable_errors;
                    result.complete = false;
                    error.clear();
                }
                media.modified_unix = modified_unix(entry.last_write_time(error));
                if (error) {
                    media.modified_unix = 0;
                    ++result.recoverable_errors;
                    result.complete = false;
                    error.clear();
                }
                ++result.media_items;
                if (!visitor(media)) {
                    result.complete = false;
                    break;
                }
            }
        } else if (error) {
            ++result.recoverable_errors;
            result.complete = false;
            error.clear();
        }

        current.increment(error);
        if (error) {
            ++result.recoverable_errors;
            result.complete = false;
            error.clear();
        }
    }
    if (current != end || reached_limit(result, limits) ||
        (cancelled && cancelled())) {
        result.complete = false;
    }
#else
    struct stat root_stat {};
    if (lstat(root.c_str(), &root_stat) != 0) {
        result.fatal_errno = errno;
        result.fatal_path = root;
        return result;
    }
    if (!S_ISDIR(root_stat.st_mode) || S_ISLNK(root_stat.st_mode)) {
        result.fatal_errno = ENOTDIR;
        result.fatal_path = root;
        return result;
    }

    struct Directory {
        std::string path;
        std::size_t depth = 0;
    };
    std::vector<Directory> pending{{root, 0}};

    while (!pending.empty() && !reached_limit(result, limits) &&
           (!cancelled || !cancelled())) {
        Directory directory = std::move(pending.back());
        pending.pop_back();
        int directory_error = 0;
        bool different_device = false;
        DIR* handle = open_directory_no_follow(
            directory.path,
            root_stat.st_dev,
            directory_error,
            different_device);
        if (!handle) {
            const int value = directory_error;
            if (different_device) {
                ++result.skipped_devices;
                continue;
            }
            if (fatal_io_errno(value)) {
                result.fatal_errno = value;
                result.fatal_path = directory.path;
                return result;
            }
            record_recoverable(result, directory.path, value);
            continue;
        }

        errno = 0;
        const int directory_descriptor = dirfd(handle);
        if (directory_descriptor < 0) {
            const int value = errno != 0 ? errno : EBADF;
            closedir(handle);
            if (fatal_io_errno(value)) {
                result.fatal_errno = value;
                result.fatal_path = directory.path;
                return result;
            }
            record_recoverable(result, directory.path, value);
            continue;
        }

        int read_error = 0;
        for (;;) {
            errno = 0;
            dirent* item = readdir(handle);
            if (!item) {
                read_error = errno;
                break;
            }
            if (cancelled && cancelled()) {
                closedir(handle);
                result.complete = false;
                return result;
            }
            if (std::strcmp(item->d_name, ".") == 0 ||
                std::strcmp(item->d_name, "..") == 0) {
                continue;
            }
            if (reached_limit(result, limits)) {
                break;
            }

            std::string path = directory.path;
            if (!path.empty() && path.back() != '/') {
                path.push_back('/');
            }
            path += item->d_name;
            ++result.entries_seen;

            struct stat status {};
            int status_error = 0;
            if (!stat_directory_entry(
                    directory_descriptor,
                    path,
                    item->d_name,
                    status,
                    status_error)) {
                const int value = status_error;
                if (fatal_io_errno(value)) {
                    closedir(handle);
                    result.fatal_errno = value;
                    result.fatal_path = path;
                    return result;
                }
                record_recoverable(result, path, value);
                continue;
            }
            if (S_ISLNK(status.st_mode)) {
                ++result.skipped_symlinks;
                continue;
            }
            if (status.st_dev != root_stat.st_dev) {
                ++result.skipped_devices;
                continue;
            }
            if (S_ISDIR(status.st_mode)) {
                if (skip_storage_metadata_directory(item->d_name)) {
                    continue;
                }
                if (directory.depth + 1 < limits.max_depth) {
                    pending.push_back({std::move(path), directory.depth + 1});
                }
                continue;
            }
            if (!S_ISREG(status.st_mode)) {
                continue;
            }

            const MediaKind kind = classify_media_path(path);
            if (kind == MediaKind::unknown || kind == MediaKind::subtitle) {
                continue;
            }
            MediaEntry media{};
            media.path = std::move(path);
            media.name = item->d_name;
            media.kind = kind;
            media.size = status.st_size > 0 ? static_cast<std::uint64_t>(status.st_size) : 0;
            media.modified_unix = static_cast<std::int64_t>(status.st_mtime);
            ++result.media_items;
            if (!visitor(media)) {
                closedir(handle);
                result.complete = false;
                return result;
            }
        }

        closedir(handle);
        if (read_error != 0) {
            if (fatal_io_errno(read_error)) {
                result.fatal_errno = read_error;
                result.fatal_path = directory.path;
                return result;
            }
            record_recoverable(result, directory.path, read_error);
        }
    }
    if (!pending.empty() || reached_limit(result, limits) ||
        (cancelled && cancelled())) {
        result.complete = false;
    }
#endif

    return result;
}

} // namespace bfplayer
