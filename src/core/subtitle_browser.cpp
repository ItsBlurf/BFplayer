#include "bfplayer/subtitle_browser.hpp"

#include "bfplayer/library_scanner.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#include <filesystem>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bfplayer {
namespace {

constexpr std::size_t kMaximumEntriesSeen = 50000;
constexpr std::size_t kMaximumVisibleEntries = 10000;
constexpr std::size_t kMaximumPathBytes = 4096;

std::string normalize_path(std::string path) {
    if (path.empty()) {
        return "/";
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

bool ignored_directory(const std::string& name) {
    std::string folded = name;
    std::transform(
        folded.begin(),
        folded.end(),
        folded.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    return folded == "$recycle.bin" ||
           folded == "system volume information" ||
           folded == ".spotlight-v100" ||
           folded == ".trashes" ||
           folded == "lost+found";
}

#if !defined(_WIN32)
bool fatal_io_error(int value) {
    return value == EIO || value == ESTALE || value == EBADF ||
           value == EFAULT;
}
#endif

} // namespace

std::string subtitle_browser_parent(const std::string& raw_path) {
    const std::string path = normalize_path(raw_path);
    if (path == "/") {
        return "/";
    }
    const std::size_t slash = path.find_last_of('/');
    return slash == 0 || slash == std::string::npos
        ? "/"
        : path.substr(0, slash);
}

SubtitleBrowserResult list_subtitle_directory(
    const std::string& requested_path) {
    SubtitleBrowserResult output;
    output.path = normalize_path(requested_path);

#if defined(_WIN32)
    std::error_code status_error;
    const std::filesystem::path root =
        std::filesystem::path(output.path);
    const auto root_status =
        std::filesystem::symlink_status(root, status_error);
    if (status_error || !std::filesystem::is_directory(root_status) ||
        std::filesystem::is_symlink(root_status)) {
        output.error = "Unable to open subtitle folder";
        return output;
    }
    std::size_t seen = 0;
    std::error_code iterator_error;
    std::filesystem::directory_iterator iterator(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        iterator_error);
    const std::filesystem::directory_iterator end;
    while (!iterator_error && iterator != end) {
        if (++seen > kMaximumEntriesSeen) {
            output.error = "Subtitle folder is too large";
            return output;
        }
        const auto status = iterator->symlink_status(status_error);
        if (status_error || std::filesystem::is_symlink(status)) {
            if (status_error) {
                ++output.unreadable_entries;
            }
            status_error.clear();
            iterator.increment(iterator_error);
            continue;
        }
        const std::string name = iterator->path().filename().string();
        const std::string path = iterator->path().string();
        if (path.size() <= kMaximumPathBytes &&
            std::filesystem::is_directory(status) &&
            !ignored_directory(name)) {
            output.entries.push_back({name, path, true});
        } else if (path.size() <= kMaximumPathBytes &&
                   std::filesystem::is_regular_file(status) &&
                   classify_media_path(path) == MediaKind::subtitle) {
            output.entries.push_back({name, path, false});
        }
        if (output.entries.size() > kMaximumVisibleEntries) {
            output.error = "Subtitle folder has too many matching files";
            return output;
        }
        iterator.increment(iterator_error);
    }
    if (iterator_error) {
        output.error = "Unable to read subtitle folder";
        return output;
    }
    output.entries_seen = seen;
#else
    struct stat requested_status {};
    if (lstat(output.path.c_str(), &requested_status) != 0 ||
        !S_ISDIR(requested_status.st_mode) ||
        S_ISLNK(requested_status.st_mode)) {
        output.error =
            "Unable to open subtitle folder (errno " +
            std::to_string(errno != 0 ? errno : ENOTDIR) + ")";
        return output;
    }
    // Match BFpilot's proven one-directory picker path. Some PS5 filesystems
    // reject fstatat for every child; the full-path lstat fallback below is
    // therefore required even when readdir succeeds.
    DIR* directory = opendir(output.path.c_str());
    if (!directory) {
        const int value = errno;
        output.error =
            "Unable to read subtitle folder (errno " +
            std::to_string(value) + ")";
        return output;
    }
    const int directory_descriptor = dirfd(directory);
    if (directory_descriptor < 0) {
        const int value = errno != 0 ? errno : EBADF;
        closedir(directory);
        output.error =
            "Unable to access subtitle folder entries (errno " +
            std::to_string(value) + ")";
        return output;
    }
    struct stat opened_status {};
    if (fstat(directory_descriptor, &opened_status) != 0 ||
        opened_status.st_dev != requested_status.st_dev ||
        opened_status.st_ino != requested_status.st_ino ||
        !S_ISDIR(opened_status.st_mode)) {
        const int value = errno != 0 ? errno : ESTALE;
        closedir(directory);
        output.error =
            "Subtitle folder changed while opening (errno " +
            std::to_string(value) + ")";
        return output;
    }
    std::size_t seen = 0;
    std::size_t stat_failures = 0;
    std::size_t stat_fallbacks = 0;
    int last_stat_error = 0;
    int fatal_error = 0;
    for (;;) {
        errno = 0;
        dirent* item = readdir(directory);
        if (!item) {
            fatal_error = errno;
            break;
        }
        if (std::strcmp(item->d_name, ".") == 0 ||
            std::strcmp(item->d_name, "..") == 0) {
            continue;
        }
        if (++seen > kMaximumEntriesSeen) {
            fatal_error = EOVERFLOW;
            break;
        }
        std::string path = output.path;
        if (path != "/") {
            path.push_back('/');
        }
        path += item->d_name;
        if (path.size() > kMaximumPathBytes) {
            continue;
        }
        struct stat status {};
        if (fstatat(
                directory_descriptor,
                item->d_name,
                &status,
                AT_SYMLINK_NOFOLLOW) != 0) {
            int value = errno;
            ++stat_failures;
            last_stat_error = value;
            if (fatal_io_error(value) ||
                lstat(path.c_str(), &status) != 0) {
                if (!fatal_io_error(value)) {
                    value = errno;
                }
                if (!fatal_io_error(value)) {
                    ++output.unreadable_entries;
                    continue;
                }
                fatal_error = value;
                break;
            }
            ++stat_fallbacks;
        }
        if (S_ISLNK(status.st_mode)) {
            continue;
        }
        if (S_ISDIR(status.st_mode) && !ignored_directory(item->d_name)) {
            output.entries.push_back({item->d_name, std::move(path), true});
        } else if (
            S_ISREG(status.st_mode) &&
            classify_media_path(path) == MediaKind::subtitle) {
            output.entries.push_back({item->d_name, std::move(path), false});
        }
        if (output.entries.size() > kMaximumVisibleEntries) {
            fatal_error = EOVERFLOW;
            break;
        }
    }
    const int close_error = closedir(directory) == 0 ? 0 : errno;
    if (fatal_error != 0 || close_error != 0) {
        output.error =
            "Unable to read subtitle folder (errno " +
            std::to_string(fatal_error != 0 ? fatal_error : close_error) +
            ")";
        output.entries.clear();
        return output;
    }
    if (seen > 0 && stat_failures == seen && stat_fallbacks == 0) {
        output.error =
            "Unable to inspect subtitle folder entries (errno " +
            std::to_string(
                last_stat_error != 0 ? last_stat_error : EIO) +
            ")";
        output.entries.clear();
        return output;
    }
    output.entries_seen = seen;
    output.stat_fallbacks = stat_fallbacks;
#endif

    for (const SubtitleBrowserEntry& entry : output.entries) {
        if (entry.directory) {
            ++output.directories;
        } else {
            ++output.subtitle_files;
        }
    }
    std::sort(
        output.entries.begin(),
        output.entries.end(),
        [](const SubtitleBrowserEntry& left,
           const SubtitleBrowserEntry& right) {
            if (left.directory != right.directory) {
                return left.directory;
            }
            return natural_path_less(left.name, right.name);
        });
    return output;
}

} // namespace bfplayer
