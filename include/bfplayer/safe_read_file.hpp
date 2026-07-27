#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bfplayer {

// Read-only regular-file handle used by FFmpeg callbacks. Opening rejects a
// symlink/reparse-point final component and verifies the opened object before it
// becomes usable. Any read/seek failure retires the handle so a bad descriptor
// is never retried.
class SafeReadFile {
public:
    SafeReadFile() = default;
    ~SafeReadFile();

    SafeReadFile(const SafeReadFile&) = delete;
    SafeReadFile& operator=(const SafeReadFile&) = delete;

    bool open(const std::string& path, std::string& error);
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] int last_error_code() const noexcept {
        return last_error_code_;
    }

    // Returns bytes read, zero at EOF, or a negative errno-style value.
    int read(std::uint8_t* buffer, int length) noexcept;

    // Uses SEEK_SET/SEEK_CUR/SEEK_END and returns the new offset, or a
    // negative errno-style value.
    std::int64_t seek(std::int64_t offset, int whence) noexcept;

private:
    void retire() noexcept;

#if defined(_WIN32)
    void* handle_ = nullptr;
#else
    int descriptor_ = -1;
#endif
    std::uint64_t size_ = 0;
    int last_error_code_ = 0;
};

} // namespace bfplayer
