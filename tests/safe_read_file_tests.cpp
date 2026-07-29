#include "bfplayer/safe_read_file.hpp"

#include <array>
#include <chrono>
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

std::string utf8_path(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size()};
}

} // namespace

int main() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("bfplayer-safe-file-" + std::to_string(suffix));
    std::error_code error;
    std::filesystem::create_directories(root, error);
    const std::filesystem::path regular = root / "sample.bin";
    {
        std::ofstream output(regular, std::ios::binary);
        output << "abcdef";
    }

    bfplayer::SafeReadFile file;
    std::string open_error;
    check(file.open(utf8_path(regular), open_error), "regular file opens");
    check(file.size() == 6, "regular file size captured");
    std::array<std::uint8_t, 4> buffer{};
    check(file.read(buffer.data(), 3) == 3, "first read succeeds");
    check(std::string(buffer.begin(), buffer.begin() + 3) == "abc",
          "first read data matches");
    check(file.seek(-2, SEEK_END) == 4, "seek from end succeeds");
    check(file.read(buffer.data(), 4) == 2, "short EOF read succeeds");
    check(std::string(buffer.begin(), buffer.begin() + 2) == "ef",
          "EOF read data matches");
    check(file.read(buffer.data(), 4) == 0, "EOF returns zero");
    check(file.read(nullptr, 1) < 0, "null destination is rejected");
    check(file.is_open(), "invalid caller buffer does not retire a valid handle");
    check(file.seek(0, 12345) < 0, "invalid seek origin is rejected");
    check(file.is_open(), "invalid seek origin does not retire a valid handle");
    const bfplayer::SafeReadFileStats stats = file.stats();
    check(stats.bytes_read == 5, "read telemetry counts delivered bytes");
    check(stats.read_calls == 4, "read telemetry counts all calls");
    check(stats.seek_calls == 2, "seek telemetry counts all calls");
    file.close();
    check(file.read(buffer.data(), 1) < 0, "closed handle cannot be reused");
    check(file.last_error_code() != 0, "closed-handle error is recorded");

    check(!file.open(utf8_path(root), open_error), "directory is rejected");

    const std::filesystem::path link = root / "sample-link.bin";
    std::filesystem::create_symlink(regular, link, error);
    if (!error) {
        check(!file.open(utf8_path(link), open_error), "symlink is rejected");
    }

    std::filesystem::remove_all(root, error);
    if (failures == 0) {
        std::cout << "safe_read_file_tests: PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
