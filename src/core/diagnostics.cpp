#include "ps5mc/diagnostics.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#if defined(PS5MC_PS5)
extern "C" {
#include <libavutil/log.h>
}
#endif

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace ps5mc {
namespace {

constexpr std::size_t kMaximumLogBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumMessageBytes = 4096U;
constexpr char kLogDirectory[] = "/data/PS5-MediaCenter/logs";
constexpr char kLatestLog[] = "/data/PS5-MediaCenter/logs/latest.log";
constexpr char kPreviousLog[] = "/data/PS5-MediaCenter/logs/previous.log";

std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
FILE* g_file = nullptr;
std::size_t g_bytes = 0;
bool g_truncated = false;
bool g_initialized = false;
#if !defined(_WIN32)
int g_crash_descriptor = -1;
#endif

void lock() noexcept {
    while (g_lock.test_and_set(std::memory_order_acquire)) {
    }
}

void unlock() noexcept {
    g_lock.clear(std::memory_order_release);
}

const char* level_name(DiagnosticLevel level) noexcept {
    switch (level) {
        case DiagnosticLevel::debug:
            return "DEBUG";
        case DiagnosticLevel::info:
            return "INFO";
        case DiagnosticLevel::warning:
            return "WARN";
        case DiagnosticLevel::error:
            return "ERROR";
    }
    return "INFO";
}

void make_single_line(char* value) noexcept {
    if (!value) {
        return;
    }
    for (char* cursor = value; *cursor; ++cursor) {
        const unsigned char character = static_cast<unsigned char>(*cursor);
        if (character < 0x20U && *cursor != '\t') {
            *cursor = ' ';
        }
    }
}

void utc_timestamp(char* output, std::size_t capacity) noexcept {
    if (!output || capacity == 0) {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - seconds).count();
    const std::time_t value = std::chrono::system_clock::to_time_t(seconds);
    std::tm broken_down{};
#if defined(_WIN32)
    gmtime_s(&broken_down, &value);
#else
    gmtime_r(&value, &broken_down);
#endif
    std::snprintf(
        output,
        capacity,
        "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
        broken_down.tm_year + 1900,
        broken_down.tm_mon + 1,
        broken_down.tm_mday,
        broken_down.tm_hour,
        broken_down.tm_min,
        broken_down.tm_sec,
        static_cast<long long>(milliseconds));
}

void write_locked(const char* data, std::size_t length) noexcept {
    if (!g_file || !data || length == 0 || g_bytes >= kMaximumLogBytes) {
        return;
    }
    const std::size_t available = kMaximumLogBytes - g_bytes;
    const std::size_t amount = std::min(length, available);
    const std::size_t written = std::fwrite(data, 1, amount, g_file);
    g_bytes += written;
    std::fflush(g_file);
    if (written < length || g_bytes >= kMaximumLogBytes) {
        g_truncated = true;
    }
}

#if !defined(_WIN32)
void crash_signal_handler(int signal_number) noexcept {
    if (g_crash_descriptor >= 0) {
        static constexpr char marker[] =
            "\nPS5MC_FATAL_SIGNAL: application terminated by a fatal signal\n";
        (void)::write(g_crash_descriptor, marker, sizeof(marker) - 1U);
    }
    std::_Exit(128 + signal_number);
}

void install_signal_handlers() noexcept {
    (void)::signal(SIGABRT, crash_signal_handler);
    (void)::signal(SIGBUS, crash_signal_handler);
    (void)::signal(SIGFPE, crash_signal_handler);
    (void)::signal(SIGILL, crash_signal_handler);
    (void)::signal(SIGSEGV, crash_signal_handler);
}
#endif

#if defined(PS5MC_PS5)
void ffmpeg_log_callback(
    void* context,
    int level,
    const char* format,
    va_list arguments) {
    (void)context;
    if (level > AV_LOG_WARNING || !format) {
        return;
    }
    char message[kMaximumMessageBytes]{};
    int print_prefix = 1;
    (void)av_log_format_line2(
        nullptr,
        level,
        format,
        arguments,
        message,
        static_cast<int>(sizeof(message)),
        &print_prefix);
    make_single_line(message);
    diagnostics_log(
        level <= AV_LOG_ERROR ? DiagnosticLevel::error : DiagnosticLevel::warning,
        "ffmpeg: %s",
        message);
}
#endif

} // namespace

void diagnostics_init(int argc, char** argv) noexcept {
    lock();
    if (g_initialized) {
        unlock();
        return;
    }

#if defined(_WIN32)
    (void)_mkdir("logs");
    (void)_mkdir("logs/diagnostics");
#else
    (void)::mkdir("/data/PS5-MediaCenter", 0777);
    (void)::mkdir(kLogDirectory, 0777);
    (void)::remove(kPreviousLog);
    (void)::rename(kLatestLog, kPreviousLog);
#endif
    g_file = std::fopen(kLatestLog, "wb");
    if (!g_file) {
        // Host builds and unusual launch directories still retain stderr
        // diagnostics; the PS5 path above is the persistent path.
        g_file = std::fopen("ps5-media-center-latest.log", "wb");
    }
#if !defined(_WIN32)
    if (g_file) {
        g_crash_descriptor = ::fileno(g_file);
    }
    install_signal_handlers();
#endif
    g_initialized = true;
    unlock();

    diagnostics_log(
        DiagnosticLevel::info,
        "diagnostics-start version=1 max_bytes=%zu log_path=%s",
        kMaximumLogBytes,
        diagnostics_log_path());
#if !defined(_WIN32)
    struct utsname system_name{};
    if (::uname(&system_name) == 0) {
        diagnostics_log(
            DiagnosticLevel::info,
            "system sysname=%s release=%s version=%s machine=%s",
            system_name.sysname,
            system_name.release,
            system_name.version,
            system_name.machine);
    }
    struct statvfs storage{};
    if (::statvfs("/data", &storage) == 0) {
        diagnostics_log(
            DiagnosticLevel::info,
            "storage path=/data block_size=%lu available_bytes=%llu total_bytes=%llu",
            static_cast<unsigned long>(storage.f_bsize),
            static_cast<unsigned long long>(storage.f_bavail) *
                static_cast<unsigned long long>(storage.f_bsize),
            static_cast<unsigned long long>(storage.f_blocks) *
                static_cast<unsigned long long>(storage.f_bsize));
    } else {
        diagnostics_log(
            DiagnosticLevel::warning,
            "storage statvfs failed path=/data errno=%d",
            errno);
    }
    char cwd[1024]{};
    diagnostics_log(
        DiagnosticLevel::info,
        "process pid=%ld cwd=%s page_size=%ld argc=%d",
        static_cast<long>(::getpid()),
        ::getcwd(cwd, sizeof(cwd)) ? cwd : "<unavailable>",
        ::sysconf(_SC_PAGESIZE),
        argc);
#else
    diagnostics_log(
        DiagnosticLevel::info,
        "process pid=%ld argc=%d",
        static_cast<long>(_getpid()),
        argc);
#endif
    for (int index = 0; argv && index < argc; ++index) {
        diagnostics_log(DiagnosticLevel::info, "argv[%d]=%s", index, argv[index] ? argv[index] : "<null>");
    }
}

void diagnostics_install_ffmpeg() noexcept {
#if defined(PS5MC_PS5)
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(ffmpeg_log_callback);
    diagnostics_log(DiagnosticLevel::info, "ffmpeg logging installed level=warning");
#else
    diagnostics_log(DiagnosticLevel::info, "ffmpeg logging unavailable in host build");
#endif
}

void diagnostics_log_v(
    DiagnosticLevel level,
    const char* format,
    std::va_list arguments) noexcept {
    if (!format) {
        return;
    }
    char message[kMaximumMessageBytes]{};
    (void)std::vsnprintf(message, sizeof(message), format, arguments);
    make_single_line(message);

    char timestamp[64]{};
    utc_timestamp(timestamp, sizeof(timestamp));
    char line[kMaximumMessageBytes + 128U]{};
    const int length = std::snprintf(
        line,
        sizeof(line),
        "%s [%s] %s\n",
        timestamp,
        level_name(level),
        message);
    if (length <= 0) {
        return;
    }

    lock();
    if (g_file) {
        write_locked(line, static_cast<std::size_t>(std::min<int>(length, sizeof(line) - 1U)));
        if (g_truncated && g_bytes == kMaximumLogBytes) {
            // The cap is intentional: keep the tail from becoming an I/O
            // denial-of-service during a noisy decoder failure.
            g_truncated = false;
            const char marker[] = "PS5MC_LOG_TRUNCATED: maximum size reached\n";
            if (g_bytes + sizeof(marker) - 1U <= kMaximumLogBytes) {
                write_locked(marker, sizeof(marker) - 1U);
            }
        }
    }
    unlock();

    std::fputs(line, stderr);
    std::fflush(stderr);
}

void diagnostics_log(DiagnosticLevel level, const char* format, ...) noexcept {
    va_list arguments;
    va_start(arguments, format);
    diagnostics_log_v(level, format, arguments);
    va_end(arguments);
}

void diagnostics_flush() noexcept {
    lock();
    if (g_file) {
        std::fflush(g_file);
    }
    unlock();
}

void diagnostics_shutdown() noexcept {
    lock();
    if (g_file) {
        std::fflush(g_file);
        std::fclose(g_file);
        g_file = nullptr;
    }
#if !defined(_WIN32)
    g_crash_descriptor = -1;
#endif
    g_initialized = false;
    unlock();
}

const char* diagnostics_log_path() noexcept {
    return kLatestLog;
}

} // namespace ps5mc
