#include "ps5mc/bulk_import.hpp"

#include "ps5mc/library_scanner.hpp"
#include "ps5mc/library_view.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <filesystem>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ps5mc {
namespace {

constexpr std::size_t kMaximumDirectEntries = 10000;
constexpr std::size_t kMaximumBulkEntries = 200000;

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
#endif

bool folder_contains_video(const std::string& path, BulkImportResult& output) {
    if (output.entries_checked >= kMaximumBulkEntries) {
        output.fatal_errno = EOVERFLOW;
        output.fatal_path = path;
        return false;
    }
    const std::size_t remaining_entries =
        kMaximumBulkEntries - output.entries_checked;
    bool found = false;
    const ScanResult scan = scan_media_library(
        path,
        [&](const MediaEntry& entry) {
            if (entry.kind == MediaKind::video) {
                found = true;
                return false;
            }
            return true;
        },
        ScanLimits{32, remaining_entries, 75000});
    output.entries_checked += scan.entries_seen;
    output.skipped_symlinks += scan.skipped_symlinks;
    output.skipped_devices += scan.skipped_devices;
    if (scan.fatal_errno != 0) {
        output.fatal_errno = scan.fatal_errno;
        output.fatal_path = scan.fatal_path;
        return false;
    }
    // The scanner reports incomplete when our visitor intentionally stops at
    // the first video. Any other incomplete result would make the folder's
    // classification unreliable, so fail the bulk import instead of silently
    // returning a partial library.
    if (!found && !scan.fully_enumerated()) {
        output.fatal_errno =
            scan.first_recoverable_errno != 0
                ? scan.first_recoverable_errno
                : EOVERFLOW;
        output.fatal_path =
            scan.first_recoverable_path.empty()
                ? path
                : scan.first_recoverable_path;
        return false;
    }
    return found;
}

void add_movie(BulkImportResult& output, std::string path) {
    output.sources.push_back({
        MediaSourceKind::movie_file,
        std::move(path),
        {}});
    output.sources.back().title =
        media_source_default_title(output.sources.back().path);
    ++output.loose_movies;
}

void add_show(BulkImportResult& output, std::string path) {
    output.sources.push_back({
        MediaSourceKind::tv_folder,
        std::move(path),
        {}});
    output.sources.back().title =
        media_source_default_title(output.sources.back().path);
    ++output.tv_folders;
}

#if defined(_WIN32)

std::filesystem::path utf8_filesystem_path(const std::string& path) {
    const std::u8string utf8(
        reinterpret_cast<const char8_t*>(path.data()),
        reinterpret_cast<const char8_t*>(path.data() + path.size()));
    return std::filesystem::path(utf8);
}

std::string utf8_path(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(value.data()),
        value.size());
}

bool is_reparse_point(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES ||
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

#endif

} // namespace

BulkImportResult discover_bulk_media_sources(
    const std::string& selected_root) {
    BulkImportResult output{};
    const std::string root = normalize_media_source_path(selected_root);
    if (root.empty() || root == "/") {
        output.fatal_errno = EINVAL;
        output.fatal_path = root;
        return output;
    }

#if defined(_WIN32)
    std::error_code error;
    const std::filesystem::path root_path = utf8_filesystem_path(root);
    if (is_reparse_point(root_path) ||
        !std::filesystem::is_directory(root_path, error) || error) {
        output.fatal_errno = error ? error.value() : ENOTDIR;
        output.fatal_path = root;
        return output;
    }
    std::filesystem::directory_iterator current(
        root_path,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::directory_iterator end;
    if (error) {
        output.fatal_errno = error.value();
        output.fatal_path = root;
        return output;
    }
    std::size_t direct_entries = 0;
    while (current != end) {
        if (direct_entries >= kMaximumDirectEntries ||
            output.entries_checked >= kMaximumBulkEntries) {
            output.fatal_errno = EOVERFLOW;
            output.fatal_path = root;
            return output;
        }
        const std::filesystem::directory_entry entry = *current;
        ++direct_entries;
        ++output.entries_checked;
        const std::string path = utf8_path(entry.path());
        if (is_reparse_point(entry.path())) {
            ++output.skipped_symlinks;
        } else if (entry.is_regular_file(error) && !error) {
            if (classify_media_path(path) == MediaKind::video) {
                add_movie(output, path);
            }
        } else if (!error && entry.is_directory(error) && !error &&
                   folder_contains_video(path, output)) {
            add_show(output, path);
        }
        if (!output.ok()) {
            return output;
        }
        error.clear();
        current.increment(error);
        if (error) {
            output.fatal_errno = error.value();
            output.fatal_path = path;
            return output;
        }
    }
#else
    struct stat root_status {};
    if (lstat(root.c_str(), &root_status) != 0 ||
        !S_ISDIR(root_status.st_mode) || S_ISLNK(root_status.st_mode)) {
        output.fatal_errno = errno != 0 ? errno : ENOTDIR;
        output.fatal_path = root;
        return output;
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
    const int descriptor = open(root.c_str(), flags);
    if (descriptor < 0) {
        output.fatal_errno = errno;
        output.fatal_path = root;
        return output;
    }
    struct stat opened_status {};
    if (fstat(descriptor, &opened_status) != 0 ||
        opened_status.st_dev != root_status.st_dev ||
        opened_status.st_ino != root_status.st_ino) {
        output.fatal_errno = errno != 0 ? errno : ESTALE;
        output.fatal_path = root;
        close(descriptor);
        return output;
    }
    DIR* directory = fdopendir(descriptor);
    if (!directory) {
        output.fatal_errno = errno;
        output.fatal_path = root;
        close(descriptor);
        return output;
    }
    std::size_t direct_entries = 0;
    for (;;) {
        errno = 0;
        dirent* item = readdir(directory);
        if (!item) {
            if (errno != 0) {
                output.fatal_errno = errno;
                output.fatal_path = root;
            }
            break;
        }
        if (std::strcmp(item->d_name, ".") == 0 ||
            std::strcmp(item->d_name, "..") == 0) {
            continue;
        }
        if (++direct_entries > kMaximumDirectEntries ||
            ++output.entries_checked > kMaximumBulkEntries) {
            output.fatal_errno = EOVERFLOW;
            output.fatal_path = root;
            break;
        }
        std::string path = root;
        if (path.back() != '/') {
            path.push_back('/');
        }
        path += item->d_name;
        struct stat status {};
        if (fstatat(
                descriptor,
                item->d_name,
                &status,
                AT_SYMLINK_NOFOLLOW) != 0) {
            int value = errno;
            if (fatal_io_errno(value)) {
                output.fatal_errno = value;
                output.fatal_path = path;
                break;
            }
            // PS5 exFAT can enumerate an entry while descriptor-relative stat
            // rejects it. Use the scanner's stable no-follow fallback before
            // deciding that an otherwise visible USB item does not exist.
            ++output.stat_fallbacks;
            if (lstat(path.c_str(), &status) != 0) {
                value = errno;
                if (fatal_io_errno(value)) {
                    output.fatal_errno = value;
                    output.fatal_path = path;
                    break;
                }
                ++output.unreadable_entries;
                continue;
            }
        }
        if (S_ISLNK(status.st_mode)) {
            ++output.skipped_symlinks;
            continue;
        }
        if (status.st_dev != root_status.st_dev) {
            ++output.skipped_devices;
            continue;
        }
        if (S_ISREG(status.st_mode)) {
            if (classify_media_path(path) == MediaKind::video) {
                add_movie(output, std::move(path));
            }
        } else if (S_ISDIR(status.st_mode) &&
                   folder_contains_video(path, output)) {
            add_show(output, std::move(path));
        }
        if (!output.ok()) {
            break;
        }
    }
    closedir(directory);
#endif

    std::sort(
        output.sources.begin(),
        output.sources.end(),
        [](const MediaSource& left, const MediaSource& right) {
            if (left.kind != right.kind) {
                return left.kind == MediaSourceKind::tv_folder;
            }
            return natural_path_less(left.path, right.path);
        });
    return output;
}

} // namespace ps5mc
