#pragma once

#include <cstddef>
#include <cstdarg>

namespace bfplayer {

enum class DiagnosticLevel {
    debug,
    info,
    warning,
    error,
};

// Opens /data/BFplayer/logs/latest.log, rotates the previous session,
// installs crash markers, and records process/build context. Safe to call
// before SDL or Kitchensink initialization.
void diagnostics_init(int argc, char** argv) noexcept;

// Installs a bounded FFmpeg callback. Call after diagnostics_init().
void diagnostics_install_ffmpeg() noexcept;

void diagnostics_log(DiagnosticLevel level, const char* format, ...) noexcept;
void diagnostics_log_v(DiagnosticLevel level, const char* format, std::va_list arguments) noexcept;
void diagnostics_flush() noexcept;
void diagnostics_shutdown() noexcept;

[[nodiscard]] const char* diagnostics_log_path() noexcept;

} // namespace bfplayer
