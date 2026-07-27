#include "bfplayer/safe_read_file.hpp"

#include <cerrno>
#include <cstdio>
#include <limits>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace bfplayer {
namespace {

constexpr int negative_error(int value) noexcept {
    return value > 0 ? -value : -EIO;
}

#if defined(_WIN32)

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty() ||
        value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            wide.data(),
            required) != required) {
        return {};
    }
    return wide;
}

std::string windows_error(const char* operation, DWORD value) {
    return std::string(operation) + ": win32=" + std::to_string(value);
}

#endif

} // namespace

SafeReadFile::~SafeReadFile() {
    close();
}

bool SafeReadFile::open(const std::string& path, std::string& error) {
    close();
    last_error_code_ = 0;
    error.clear();
    if (path.empty() || path.find('\0') != std::string::npos) {
        last_error_code_ = EINVAL;
        error = "Invalid local file path";
        return false;
    }

#if defined(_WIN32)
    const std::wstring wide = utf8_to_wide(path);
    if (wide.empty()) {
        last_error_code_ = EINVAL;
        error = "Local file path is not valid UTF-8";
        return false;
    }
    HANDLE opened = CreateFileW(
        wide.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (opened == INVALID_HANDLE_VALUE) {
        last_error_code_ = EIO;
        error = windows_error("Open local file", GetLastError());
        return false;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    LARGE_INTEGER size{};
    const DWORD type = GetFileType(opened);
    if (type != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(opened, &information) ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        !GetFileSizeEx(opened, &size) || size.QuadPart < 0) {
        const DWORD value = GetLastError();
        CloseHandle(opened);
        last_error_code_ = EIO;
        error = windows_error("Validate local regular file", value);
        return false;
    }
    handle_ = opened;
    size_ = static_cast<std::uint64_t>(size.QuadPart);
    return true;
#else
    struct stat before {};
    if (lstat(path.c_str(), &before) != 0) {
        last_error_code_ = errno;
        error = "lstat local file: errno=" + std::to_string(errno);
        return false;
    }
    if (!S_ISREG(before.st_mode) || S_ISLNK(before.st_mode) ||
        before.st_size < 0) {
        last_error_code_ = EINVAL;
        error = "Local input is not a regular non-symlink file";
        return false;
    }

    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int opened = ::open(path.c_str(), flags);
    if (opened < 0) {
        last_error_code_ = errno;
        error = "open local file: errno=" + std::to_string(errno);
        return false;
    }
    struct stat after {};
    if (fstat(opened, &after) != 0 ||
        !S_ISREG(after.st_mode) ||
        after.st_dev != before.st_dev ||
        after.st_ino != before.st_ino ||
        after.st_size < 0) {
        const int value = errno;
        ::close(opened);
        last_error_code_ = value != 0 ? value : ESTALE;
        error = "Local file changed while opening";
        if (value != 0) {
            error += ": errno=" + std::to_string(value);
        }
        return false;
    }
    descriptor_ = opened;
    size_ = static_cast<std::uint64_t>(after.st_size);
    return true;
#endif
}

void SafeReadFile::close() noexcept {
#if defined(_WIN32)
    if (handle_) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
#else
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
#endif
    size_ = 0;
}

bool SafeReadFile::is_open() const noexcept {
#if defined(_WIN32)
    return handle_ != nullptr;
#else
    return descriptor_ >= 0;
#endif
}

void SafeReadFile::retire() noexcept {
    close();
}

int SafeReadFile::read(std::uint8_t* buffer, int length) noexcept {
    if (!is_open()) {
        last_error_code_ = EBADF;
        return -EBADF;
    }
    if (!buffer || length < 0) {
        last_error_code_ = EINVAL;
        return -EINVAL;
    }
    if (length == 0) {
        return 0;
    }
#if defined(_WIN32)
    DWORD count = 0;
    if (!ReadFile(
            static_cast<HANDLE>(handle_),
            buffer,
            static_cast<DWORD>(length),
            &count,
            nullptr)) {
        retire();
        last_error_code_ = EIO;
        return -EIO;
    }
    return static_cast<int>(count);
#else
    for (;;) {
        const ssize_t count = ::read(
            descriptor_, buffer, static_cast<std::size_t>(length));
        if (count >= 0) {
            return static_cast<int>(count);
        }
        if (errno == EINTR) {
            continue;
        }
        const int value = errno;
        retire();
        last_error_code_ = value;
        return negative_error(value);
    }
#endif
}

std::int64_t SafeReadFile::seek(std::int64_t offset, int whence) noexcept {
    if (!is_open()) {
        last_error_code_ = EBADF;
        return -EBADF;
    }
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        last_error_code_ = EINVAL;
        return -EINVAL;
    }
#if defined(_WIN32)
    DWORD method = FILE_BEGIN;
    if (whence == SEEK_CUR) {
        method = FILE_CURRENT;
    } else if (whence == SEEK_END) {
        method = FILE_END;
    }
    LARGE_INTEGER distance{};
    LARGE_INTEGER result{};
    distance.QuadPart = offset;
    if (!SetFilePointerEx(
            static_cast<HANDLE>(handle_), distance, &result, method) ||
        result.QuadPart < 0) {
        retire();
        last_error_code_ = EIO;
        return -EIO;
    }
    return result.QuadPart;
#else
    const off_t result = lseek(descriptor_, static_cast<off_t>(offset), whence);
    if (result < 0) {
        const int value = errno;
        retire();
        last_error_code_ = value;
        return negative_error(value);
    }
    return static_cast<std::int64_t>(result);
#endif
}

} // namespace bfplayer
