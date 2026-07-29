#include <SDL.h>
#include <kitchensink2/kitchensink.h>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
}

#include "bfplayer/external_subtitles.hpp"
#include "bfplayer/controller_buttons.hpp"
#include "bfplayer/diagnostics.hpp"
#include "bfplayer/library_database.hpp"
#include "bfplayer/library_scanner.hpp"
#include "bfplayer/library_ui.hpp"
#include "bfplayer/list_navigation.hpp"
#include "bfplayer/kitchensink_subtitle_timing.h"
#include "bfplayer/playlist.hpp"
#include "bfplayer/playback_osd.hpp"
#include "bfplayer/player_settings.hpp"
#include "bfplayer/remote_control.hpp"
#include "bfplayer/safe_read_file.hpp"
#include "bfplayer/source_uri.hpp"
#include "bfplayer/subtitle_browser.hpp"
#include "bfplayer/subtitle_provider.hpp"
#include "bfplayer/video_layout.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <fcntl.h>

namespace {

extern "C" int sceSystemServiceGetAppIdOfRunningBigApp(void);
extern "C" int sceSystemServiceGetAppTitleId(int app_id, char* title_id);
extern "C" int sceSystemServiceKillApp(
    int app_id,
    int how,
    int reason,
    int core_dump);

#ifndef BFPLAYER_VERSION
#define BFPLAYER_VERSION "development"
#endif

constexpr int kWindowWidth = 1920;
constexpr int kWindowHeight = 1080;
constexpr int kPreferredOutputWidth = 3840;
constexpr int kPreferredOutputHeight = 2160;
constexpr int kPreferredOutputRefresh = 60;
constexpr std::uint16_t kRemoteControlPort = 9042;
constexpr int kAudioBufferBytes = 64 * 1024;
constexpr int kSubtitleAtlasSize = 4096;
constexpr int kSubtitleFragments = 1024;
constexpr int kVideoDecoderThreads = 16;
constexpr int kVideoPacketBufferCount = 64;
constexpr int kVideoFrameBufferCount = 3;
constexpr int kNetworkVideoPacketBufferCount = 128;
constexpr int kNetworkVideoFrameBufferCount = 4;
constexpr int kAudioPacketBufferCount = 64;
constexpr int kNetworkAudioPacketBufferCount = 128;
constexpr int kAudioFrameBufferCount = 64;
constexpr std::uint64_t kAudioBytesPerSecond = 48000ULL * 2ULL * 2ULL;

struct PlaybackBufferPolicy {
    int video_packets = kVideoPacketBufferCount;
    int video_frames = kVideoFrameBufferCount;
    int audio_packets = kAudioPacketBufferCount;
    int audio_frames = kAudioFrameBufferCount;
};

PlaybackBufferPolicy playback_buffer_policy(bool network) {
    if (!network) {
        return {};
    }
    return {
        kNetworkVideoPacketBufferCount,
        kNetworkVideoFrameBufferCount,
        kNetworkAudioPacketBufferCount,
        kAudioFrameBufferCount,
    };
}

void apply_playback_buffer_policy(const PlaybackBufferPolicy& policy) {
    Kit_SetHint(KIT_HINT_VIDEO_BUFFER_PACKETS, policy.video_packets);
    Kit_SetHint(KIT_HINT_VIDEO_BUFFER_FRAMES, policy.video_frames);
    Kit_SetHint(KIT_HINT_AUDIO_BUFFER_PACKETS, policy.audio_packets);
    Kit_SetHint(KIT_HINT_AUDIO_BUFFER_FRAMES, policy.audio_frames);
}

std::uint64_t process_peak_rss_kib() noexcept {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(usage.ru_maxrss);
}

double timeval_seconds(const timeval& value) noexcept {
    return static_cast<double>(value.tv_sec) +
        static_cast<double>(value.tv_usec) / 1'000'000.0;
}

double process_cpu_seconds(const rusage& usage) noexcept {
    return timeval_seconds(usage.ru_utime) + timeval_seconds(usage.ru_stime);
}

double elapsed_performance_ms(
    Uint64 start,
    Uint64 end,
    Uint64 frequency) noexcept {
    if (end < start || frequency == 0) {
        return 0.0;
    }
    return static_cast<double>(end - start) * 1000.0 /
        static_cast<double>(frequency);
}

struct TimingWindow {
    double total_ms = 0.0;
    double maximum_ms = 0.0;
    std::uint64_t count = 0;
    std::array<std::uint32_t, 256> histogram{};

    void add(double milliseconds) noexcept {
        if (!std::isfinite(milliseconds) || milliseconds < 0.0) {
            return;
        }
        total_ms += milliseconds;
        maximum_ms = std::max(maximum_ms, milliseconds);
        ++count;
        const std::size_t bucket = static_cast<std::size_t>(
            std::clamp(milliseconds, 0.0, 255.0));
        ++histogram[bucket];
    }

    [[nodiscard]] double average() const noexcept {
        return count > 0 ? total_ms / static_cast<double>(count) : 0.0;
    }

    [[nodiscard]] double percentile(double fraction) const noexcept {
        if (count == 0) {
            return 0.0;
        }
        const std::uint64_t wanted = std::max<std::uint64_t>(
            1,
            static_cast<std::uint64_t>(
                std::ceil(static_cast<double>(count) * fraction)));
        std::uint64_t accumulated = 0;
        for (std::size_t index = 0; index < histogram.size(); ++index) {
            accumulated += histogram[index];
            if (accumulated >= wanted) {
                return static_cast<double>(index);
            }
        }
        return 255.0;
    }

    void reset() noexcept {
        total_ms = 0.0;
        maximum_ms = 0.0;
        count = 0;
        histogram.fill(0);
    }
};

enum class PlaybackOverlay {
    none,
    menu,
    controls,
    settings,
    subtitles,
    subtitle_browser,
    subtitle_providers,
    subtitle_subdl,
    subtitle_online,
};

enum class SubtitleMenuKind {
    off,
    embedded,
    external,
    browse,
    providers,
};

struct SubtitleMenuItem {
    SubtitleMenuKind kind = SubtitleMenuKind::off;
    int index = -1;
    std::string label;
};

enum class SubtitleJobType {
    none,
    search,
    download,
};

struct SubtitleJob {
    SDL_Thread* thread = nullptr;
    std::atomic<bool> done{false};
    SubtitleJobType type = SubtitleJobType::none;
    std::string api_key;
    std::string media_path;
    std::string languages;
    bool search_by_title = false;
    bfplayer::OnlineSubtitle selected;
    bfplayer::OnlineSubtitleSearch search;
    bfplayer::OnlineSubtitleDownload download;
    std::string saved_path;
    std::string error;
};

enum class TextEditMode {
    none,
    subdl_api_key,
    subtitle_languages,
    subtitle_search_query,
};

struct App {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_GameController* controller = nullptr;
    Kit_Source* source = nullptr;
    Kit_Player* player = nullptr;
    SDL_Texture* video = nullptr;
    SDL_Texture* subtitles = nullptr;
    SDL_AudioDeviceID audio = 0;
    bfplayer::ExternalSubtitles external_subtitles;
    bfplayer::SafeReadFile local_media;
    bfplayer::PlaybackOsd osd;
    bfplayer::RemoteControlServer remote_control;
    std::vector<std::string> subtitle_sidecars;
    std::string current_media_path;
    std::string fallback_font;
    std::string ui_logo;
    std::string pending_remote_media_path;
    double source_frame_rate = 0.0;
    int source_width = 0;
    int source_height = 0;
    bool source_hdr = false;
    std::string source_hdr_transfer;
    int external_subtitle_index = -1;
    int volume_percent = 100;
    int previous_volume_percent = 100;
    std::int64_t subtitle_delay_ms = 0;
    double source_display_aspect = 0.0;
    int output_width = kWindowWidth;
    int output_height = kWindowHeight;
    bool true_4k_output = false;
    bfplayer::PlayerSettings settings;
    PlaybackOverlay playback_overlay = PlaybackOverlay::none;
    int playback_overlay_selected = 0;
    bfplayer::SubtitleBrowserResult subtitle_browser;
    int subtitle_browser_selected = 0;
    int subtitle_browser_first = 0;
    std::vector<bfplayer::OnlineSubtitle> online_subtitles;
    std::string subtitle_online_error;
    SubtitleJob subtitle_job;
    TextEditMode text_edit_mode = TextEditMode::none;
    std::string text_edit_buffer;
    bfplayer::PlayerSettings text_edit_previous_settings;
    bool ime_was_visible = false;
    bfplayer::ListNavigationRepeat navigation_repeat;
    bfplayer::VideoScaleMode video_scale_mode = bfplayer::VideoScaleMode::fit;
    bfplayer::VideoAspectMode video_aspect_mode =
        bfplayer::VideoAspectMode::default_ratio;
    bfplayer::VideoCropMode video_crop_mode =
        bfplayer::VideoCropMode::default_crop;
    bool running = true;
    bool playback_running = false;
    bool paused = false;
    bool redraw_requested = true;
    std::atomic<bool> io_cancel{false};
    std::atomic<std::int64_t> source_open_deadline_ms{0};
};

void update_decoder_buffer_status(
    const App& app,
    bfplayer::RemotePlaybackStatus& status) {
    if (!app.player) {
        return;
    }
    if (Kit_GetPlayerVideoStream(app.player) >= 0) {
        Kit_GetPlayerVideoBufferState(
            app.player,
            &status.video_frames_length,
            &status.video_frames_capacity,
            &status.video_packets_length,
            &status.video_packets_capacity);
    }
    if (Kit_GetPlayerAudioStream(app.player) >= 0) {
        Kit_GetPlayerAudioBufferState(
            app.player,
            &status.audio_frames_length,
            &status.audio_frames_capacity,
            &status.audio_packets_length,
            &status.audio_packets_capacity);
    }
}

Kit_VideoToneMapInfo update_tone_map_status(
    const App& app,
    bfplayer::RemotePlaybackStatus& status) {
    Kit_VideoToneMapInfo info{};
    if (app.player) {
        Kit_GetPlayerVideoToneMapInfo(app.player, &info);
    }
    status.hdr_tone_map_active = info.active != 0;
    status.hdr_input_full_range = info.input_full_range != 0;
    status.hdr_input_bt2020 = info.input_bt2020 != 0;
    status.hdr_source_peak_nits = info.source_peak_nits;
    status.hdr_target_peak_nits = info.target_peak_nits;
    status.hdr_tone_map_frames = info.frames;
    status.hdr_tone_map_time_us = info.processing_us;
    status.hdr_tone_map_workers = info.workers;
    status.hdr_tone_map_average_ms =
        info.frames > 0
        ? static_cast<double>(info.processing_us) /
              static_cast<double>(info.frames) /
              1000.0
        : 0.0;
    return info;
}

enum class PlaybackOutcome {
    finished,
    user_return,
    error,
};

enum class PlayerLockResult {
    acquired,
    already_running,
    error,
};

void boot_stage_stderr(const char* stage) noexcept {
    std::fprintf(
        stderr,
        "BFPLAYER_BOOT_STAGE stage=%s pid=%ld\n",
        stage ? stage : "<unknown>",
        static_cast<long>(getpid()));
    std::fflush(stderr);
}

void boot_stage(const char* stage) noexcept {
    boot_stage_stderr(stage);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "boot-stage stage=%s pid=%ld",
        stage ? stage : "<unknown>",
        static_cast<long>(getpid()));
}

class PlayerInstanceLock {
public:
    PlayerInstanceLock() = default;
    PlayerInstanceLock(const PlayerInstanceLock&) = delete;
    PlayerInstanceLock& operator=(const PlayerInstanceLock&) = delete;

    ~PlayerInstanceLock() {
        if (descriptor_ >= 0) {
            (void)flock(descriptor_, LOCK_UN);
            close(descriptor_);
        }
    }

    [[nodiscard]] PlayerLockResult acquire() noexcept {
        constexpr const char* directory = "/data/BFplayer";
        constexpr const char* path = "/data/BFplayer/player.lock";
        if (mkdir(directory, 0777) != 0 && errno != EEXIST) {
            return PlayerLockResult::error;
        }
        descriptor_ = open(path, O_RDWR | O_CREAT, 0600);
        if (descriptor_ < 0) {
            return PlayerLockResult::error;
        }
        if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const int lock_error = errno;
            close(descriptor_);
            descriptor_ = -1;
            return lock_error == EWOULDBLOCK || lock_error == EAGAIN
                ? PlayerLockResult::already_running
                : PlayerLockResult::error;
        }

        char owner[64];
        const int length = std::snprintf(
            owner,
            sizeof(owner),
            "pid=%ld\n",
            static_cast<long>(getpid()));
        if (length > 0) {
            (void)ftruncate(descriptor_, 0);
            (void)lseek(descriptor_, 0, SEEK_SET);
            (void)write(
                descriptor_,
                owner,
                static_cast<std::size_t>(
                    std::min(length, static_cast<int>(sizeof(owner) - 1))));
        }
        return PlayerLockResult::acquired;
    }

private:
    int descriptor_ = -1;
};

struct VideoSourceInfo {
    const char* codec = "none";
    const char* pixel_format = "unknown";
    int profile = -1;
    int width = 0;
    int height = 0;
    int bit_depth = 0;
    int sample_aspect_numerator = 0;
    int sample_aspect_denominator = 0;
    double display_aspect = 0.0;
    double frame_rate = 0.0;
    std::int64_t bit_rate = 0;
    const char* color_range = "unknown";
    const char* color_space = "unknown";
    const char* color_transfer = "unknown";
    const char* color_primaries = "unknown";
    AVColorRange color_range_value = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace color_space_value = AVCOL_SPC_UNSPECIFIED;
    bool hdr_source = false;
    bool demanding_software_decode = false;
};

void close_media(App& app);

void log_sdl(const char* operation) {
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::error,
        "sdl operation=%s error=%s",
        operation ? operation : "<unknown>",
        SDL_GetError());
}

VideoSourceInfo inspect_video_source(
    const AVFormatContext* format,
    int stream_index) {
    VideoSourceInfo info{};
    if (!format || stream_index < 0 ||
        stream_index >= static_cast<int>(format->nb_streams)) {
        return info;
    }
    const AVStream* stream = format->streams[stream_index];
    if (!stream || !stream->codecpar) {
        return info;
    }
    const AVCodecParameters* parameters = stream->codecpar;
    info.codec = avcodec_get_name(parameters->codec_id);
    info.profile = parameters->profile;
    info.width = parameters->width;
    info.height = parameters->height;
    const AVRational guessed_aspect = av_guess_sample_aspect_ratio(
        const_cast<AVFormatContext*>(format),
        const_cast<AVStream*>(stream),
        nullptr);
    if (guessed_aspect.num > 0 && guessed_aspect.den > 0) {
        info.sample_aspect_numerator = guessed_aspect.num;
        info.sample_aspect_denominator = guessed_aspect.den;
    }
    info.display_aspect = bfplayer::display_aspect_from_sample_aspect(
        info.width,
        info.height,
        info.sample_aspect_numerator,
        info.sample_aspect_denominator);
    info.bit_rate = parameters->bit_rate > 0
        ? parameters->bit_rate
        : format->bit_rate;
    if (const char* name = av_color_range_name(parameters->color_range)) {
        info.color_range = name;
    }
    info.color_range_value = parameters->color_range;
    if (const char* name = av_color_space_name(parameters->color_space)) {
        info.color_space = name;
    }
    info.color_space_value = parameters->color_space;
    if (const char* name =
            av_color_transfer_name(parameters->color_trc)) {
        info.color_transfer = name;
    }
    if (const char* name =
            av_color_primaries_name(parameters->color_primaries)) {
        info.color_primaries = name;
    }
    info.hdr_source =
        parameters->color_trc == AVCOL_TRC_SMPTE2084 ||
        parameters->color_trc == AVCOL_TRC_ARIB_STD_B67;
    const auto pixel_format =
        static_cast<AVPixelFormat>(parameters->format);
    if (const char* name = av_get_pix_fmt_name(pixel_format)) {
        info.pixel_format = name;
    }
    if (const AVPixFmtDescriptor* descriptor =
            av_pix_fmt_desc_get(pixel_format)) {
        info.bit_depth = descriptor->comp[0].depth;
    }
    const AVRational guessed_rate =
        av_guess_frame_rate(const_cast<AVFormatContext*>(format),
                            const_cast<AVStream*>(stream), nullptr);
    if (guessed_rate.num > 0 && guessed_rate.den > 0) {
        info.frame_rate = av_q2d(guessed_rate);
    }
    const std::int64_t pixels =
        static_cast<std::int64_t>(info.width) * info.height;
    const bool expensive_codec =
        parameters->codec_id == AV_CODEC_ID_VP9 ||
        parameters->codec_id == AV_CODEC_ID_AV1 ||
        parameters->codec_id == AV_CODEC_ID_HEVC;
    info.demanding_software_decode =
        pixels >= 3840LL * 2160LL &&
        (info.frame_rate >= 50.0 ||
         (expensive_codec && info.bit_depth > 8));
    return info;
}

Kit_VideoFormatRequest make_video_format_request(
    const VideoSourceInfo& source,
    int output_width,
    int output_height) {
    Kit_VideoFormatRequest request{};
    Kit_ResetVideoFormatRequest(&request);
    if (output_width < 2 || output_height < 2) {
        output_width = kWindowWidth;
        output_height = kWindowHeight;
    }
    if (source.width < 1 || source.height < 1 ||
        (source.width <= output_width && source.height <= output_height)) {
        if (source.hdr_source) {
            request.format = SDL_PIXELFORMAT_IYUV;
        }
        return request;
    }
    const double scale = std::min(
        static_cast<double>(output_width) / source.width,
        static_cast<double>(output_height) / source.height);
    request.width = std::max(
        2,
        static_cast<int>(std::floor(source.width * scale)) & ~1);
    request.height = std::max(
        2,
        static_cast<int>(std::floor(source.height * scale)) & ~1);
    if (source.hdr_source) {
        request.format = SDL_PIXELFORMAT_IYUV;
    }
    return request;
}

void sdl_log_output(
    void*,
    int category,
    SDL_LogPriority priority,
    const char* message) {
    const bfplayer::DiagnosticLevel level =
        priority >= SDL_LOG_PRIORITY_ERROR
            ? bfplayer::DiagnosticLevel::error
            : (priority == SDL_LOG_PRIORITY_WARN
                   ? bfplayer::DiagnosticLevel::warning
                   : bfplayer::DiagnosticLevel::info);
    bfplayer::diagnostics_log(
        level,
        "sdl-log category=%d priority=%d message=%s",
        category,
        static_cast<int>(priority),
        message ? message : "<null>");
}

std::string executable_directory(const char* executable_path) {
    if (!executable_path || executable_path[0] != '/') {
        return {};
    }
    const char* slash = std::strrchr(executable_path, '/');
    if (!slash) {
        return {};
    }
    if (slash == executable_path) {
        return "/";
    }
    return std::string(executable_path, static_cast<std::size_t>(slash - executable_path));
}

std::string executable_asset_path(const char* executable_path, const char* relative_path) {
    const std::string directory = executable_directory(executable_path);
    if (directory.empty()) {
        return relative_path ? relative_path : std::string{};
    }
    return directory + "/" + (relative_path ? relative_path : "");
}

void migrate_legacy_library_database() {
    constexpr const char* legacy_database =
        "/data/PS5-" "Media-" "Center/library.db";
    constexpr const char* current_directory = "/data/BFplayer";
    constexpr const char* current_database =
        "/data/BFplayer/library.db";
    struct stat destination_status {};
    if (lstat(current_database, &destination_status) == 0) {
        return;
    }
    if (errno != ENOENT) {
        return;
    }
    struct stat source_status {};
    if (lstat(legacy_database, &source_status) != 0 ||
        !S_ISREG(source_status.st_mode) ||
        S_ISLNK(source_status.st_mode) ||
        source_status.st_size < 1) {
        return;
    }
    if ((mkdir(current_directory, 0777) != 0 && errno != EEXIST)) {
        std::fprintf(
            stderr,
            "BFplayer migration: cannot create %s errno=%d\n",
            current_directory,
            errno);
        return;
    }
    const int input = open(legacy_database, O_RDONLY | O_NOFOLLOW);
    if (input < 0) {
        std::fprintf(
            stderr,
            "BFplayer migration: cannot open legacy database errno=%d\n",
            errno);
        return;
    }
    struct stat opened_source_status {};
    if (fstat(input, &opened_source_status) != 0 ||
        !S_ISREG(opened_source_status.st_mode) ||
        opened_source_status.st_dev != source_status.st_dev ||
        opened_source_status.st_ino != source_status.st_ino) {
        std::fprintf(
            stderr,
            "BFplayer migration: legacy database changed during open\n");
        close(input);
        return;
    }
    char temporary[96]{};
    std::snprintf(
        temporary,
        sizeof(temporary),
        "%s.migrate.%ld",
        current_database,
        static_cast<long>(getpid()));
    const int output = open(
        temporary,
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
        0600);
    if (output < 0) {
        std::fprintf(
            stderr,
            "BFplayer migration: cannot create temporary database errno=%d\n",
            errno);
        close(input);
        return;
    }
    bool copied = true;
    std::array<unsigned char, 64 * 1024> buffer{};
    for (;;) {
        const ssize_t received = read(input, buffer.data(), buffer.size());
        if (received == 0) {
            break;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            copied = false;
            break;
        }
        ssize_t offset = 0;
        while (offset < received) {
            const ssize_t written = write(
                output,
                buffer.data() + offset,
                static_cast<std::size_t>(received - offset));
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                copied = false;
                break;
            }
            offset += written;
        }
        if (!copied) {
            break;
        }
    }
    if (copied && fsync(output) != 0) {
        copied = false;
    }
    close(input);
    if (close(output) != 0) {
        copied = false;
    }
    if (!copied || rename(temporary, current_database) != 0) {
        const int migration_errno = errno;
        (void)unlink(temporary);
        std::fprintf(
            stderr,
            "BFplayer migration: database copy failed errno=%d\n",
            migration_errno);
        return;
    }
    std::fprintf(
        stderr,
        "BFplayer migration: library database preserved at %s\n",
        current_database);
}

void log_installed_manifest(const char* executable_path) {
    const std::string manifest_path = executable_asset_path(executable_path, "build-manifest.json");
    FILE* manifest = std::fopen(manifest_path.c_str(), "rb");
    if (!manifest) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::warning,
            "build-manifest unavailable path=%s errno=%d",
            manifest_path.c_str(),
            errno);
        return;
    }
    char contents[4096]{};
    const std::size_t bytes = std::fread(contents, 1, sizeof(contents) - 1U, manifest);
    const bool complete = std::feof(manifest) != 0;
    std::fclose(manifest);
    contents[bytes] = '\0';
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "build-manifest bytes=%zu complete=%d content=%s",
        bytes,
        complete ? 1 : 0,
        contents);
}

bfplayer::PlayerSettings load_player_settings(
    bfplayer::LibraryDatabase& database) {
    bfplayer::PlayerSettings settings{};
    std::string value;
    int integer = 0;
    bool boolean = false;
    if (database.get_setting(
            std::string(bfplayer::kSettingVolumePercent),
            value) &&
        bfplayer::parse_setting_integer(value, 0, 100, integer)) {
        settings.volume_percent = integer;
    }
    if (database.get_setting(
            std::string(bfplayer::kSettingShortSeekSeconds),
            value) &&
        bfplayer::parse_setting_integer(value, 1, 300, integer)) {
        settings.short_seek_seconds = integer;
    }
    if (database.get_setting(
            std::string(bfplayer::kSettingLongSeekSeconds),
            value) &&
        bfplayer::parse_setting_integer(value, 1, 900, integer)) {
        settings.long_seek_seconds = integer;
    }
    if (database.get_setting(
            std::string(bfplayer::kSettingOsdDurationMs),
            value) &&
        bfplayer::parse_setting_integer(value, 500, 30000, integer)) {
        settings.osd_duration_ms = integer;
    }
    if (database.get_setting(
            std::string(bfplayer::kSettingResumePlayback),
            value) &&
        bfplayer::parse_setting_boolean(value, boolean)) {
        settings.resume_playback = boolean;
    }
    if (database.get_setting(
            std::string(bfplayer::kSettingAutoSubtitles),
            value) &&
        bfplayer::parse_setting_boolean(value, boolean)) {
        settings.auto_subtitles = boolean;
    }
    if (database.get_setting(
            std::string(bfplayer::kSettingSubdlApiKey),
            value)) {
        settings.subdl_api_key = value;
    }
    if (database.get_setting(
            std::string(bfplayer::kSettingSubtitleLanguages),
            value)) {
        settings.subtitle_languages = value;
    }
    return bfplayer::normalized_player_settings(settings);
}

std::int64_t monotonic_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string ffmpeg_error(int value) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    return av_strerror(value, buffer, sizeof(buffer)) == 0
        ? std::string(buffer)
        : "FFmpeg error " + std::to_string(value);
}

std::string bounded_metadata(const char* value, std::size_t maximum) {
    if (!value) {
        return {};
    }
    std::size_t length = 0;
    while (length < maximum && value[length] != '\0') {
        ++length;
    }
    return std::string(value, length);
}

std::int64_t seconds_to_milliseconds(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return 0;
    }
    constexpr long double kMaximumSeconds =
        static_cast<long double>(std::numeric_limits<std::int64_t>::max()) /
        1000.0L;
    if (static_cast<long double>(seconds) >= kMaximumSeconds) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(std::llround(seconds * 1000.0));
}

bool playback_completed(std::int64_t position_ms, std::int64_t duration_ms) {
    return position_ms >= 0 && duration_ms > 0 &&
        static_cast<long double>(position_ms) /
            static_cast<long double>(duration_ms) >= 0.92L;
}

int interrupt_source_io(void* opaque) {
    const auto* app = static_cast<const App*>(opaque);
    if (!app || app->io_cancel.load(std::memory_order_relaxed)) {
        return 1;
    }
    const std::int64_t deadline =
        app->source_open_deadline_ms.load(std::memory_order_relaxed);
    return deadline > 0 && monotonic_milliseconds() >= deadline ? 1 : 0;
}

int read_local_media(void* opaque, std::uint8_t* buffer, int length) {
    auto* file = static_cast<bfplayer::SafeReadFile*>(opaque);
    if (!file) {
        return AVERROR(EINVAL);
    }
    const int result = file->read(buffer, length);
    return result == 0 ? AVERROR_EOF : result;
}

std::int64_t seek_local_media(void* opaque, std::int64_t offset, int whence) {
    auto* file = static_cast<bfplayer::SafeReadFile*>(opaque);
    if (!file) {
        return AVERROR(EINVAL);
    }
    if ((whence & AVSEEK_SIZE) == AVSEEK_SIZE) {
        return file->size() >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            ? AVERROR(EOVERFLOW)
            : static_cast<std::int64_t>(file->size());
    }
    const int origin = whence & ~AVSEEK_FORCE;
    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) {
        return AVERROR(EINVAL);
    }
    return file->seek(offset, origin);
}

void free_custom_avio(AVIOContext*& context) {
    if (context) {
        av_freep(&context->buffer);
        avio_context_free(&context);
    }
}

Kit_Source* create_bounded_source(
    App& app,
    const char* path,
    std::string& error) {
    error.clear();
    if (!path || !path[0]) {
        error = "Empty media source";
        bfplayer::diagnostics_log(bfplayer::DiagnosticLevel::error, "source-open rejected reason=empty");
        return nullptr;
    }
    const bool network = bfplayer::is_network_uri(path);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "source-open begin kind=%s path=%s",
        network ? "network" : "local",
        bfplayer::redact_uri_secrets(path).c_str());
    if (network && !bfplayer::is_supported_stream_uri(path)) {
        error = "Unsupported network protocol";
        bfplayer::diagnostics_log(bfplayer::DiagnosticLevel::error, "source-open rejected reason=unsupported-protocol");
        return nullptr;
    }
    if (network && bfplayer::uri_has_credentials(path)) {
        error = "Network URLs containing usernames or passwords are rejected";
        bfplayer::diagnostics_log(bfplayer::DiagnosticLevel::error, "source-open rejected reason=credentials");
        return nullptr;
    }

    app.local_media.close();
    app.io_cancel.store(false, std::memory_order_relaxed);
    app.source_open_deadline_ms.store(
        monotonic_milliseconds() + 30000,
        std::memory_order_relaxed);

    AVFormatContext* format = avformat_alloc_context();
    AVIOContext* custom_io = nullptr;
    if (!format) {
        error = "Unable to allocate FFmpeg source context";
        app.source_open_deadline_ms.store(0, std::memory_order_relaxed);
        return nullptr;
    }
    format->interrupt_callback.callback = interrupt_source_io;
    format->interrupt_callback.opaque = &app;

    if (!network) {
        if (!app.local_media.open(path, error)) {
            avformat_free_context(format);
            app.source_open_deadline_ms.store(0, std::memory_order_relaxed);
            return nullptr;
        }
        constexpr int kAvioBufferBytes = 64 * 1024;
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(kAvioBufferBytes));
        if (!buffer) {
            error = "Unable to allocate local media I/O buffer";
            app.local_media.close();
            avformat_free_context(format);
            app.source_open_deadline_ms.store(0, std::memory_order_relaxed);
            return nullptr;
        }
        custom_io = avio_alloc_context(
            buffer,
            kAvioBufferBytes,
            0,
            &app.local_media,
            read_local_media,
            nullptr,
            seek_local_media);
        if (!custom_io) {
            av_free(buffer);
            error = "Unable to allocate local media AVIO context";
            app.local_media.close();
            avformat_free_context(format);
            app.source_open_deadline_ms.store(0, std::memory_order_relaxed);
            return nullptr;
        }
        custom_io->seekable = AVIO_SEEKABLE_NORMAL;
        format->pb = custom_io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
    }

    AVDictionary* options = nullptr;
    av_dict_set(&options, "probesize", "33554432", 0);
    av_dict_set(&options, "analyzeduration", "15000000", 0);
    av_dict_set(&options, "max_probe_packets", "10000", 0);
    if (network) {
        av_dict_set(&options, "rw_timeout", "15000000", 0);
        av_dict_set(&options, "reconnect", "1", 0);
        av_dict_set(&options, "reconnect_streamed", "1", 0);
        av_dict_set(&options, "reconnect_on_network_error", "1", 0);
        av_dict_set(&options, "reconnect_delay_max", "5", 0);
        av_dict_set(
            &options,
            "protocol_whitelist",
            "async,cache,crypto,data,ftp,http,httpproxy,https,mmsh,mmst,"
            "rtmp,rtmpe,rtmps,rtmpt,rtmpte,rtmpts,rtp,rtsp,sctp,srtp,"
            "tcp,tls,udp,udplite",
            0);
    } else {
        av_dict_set(&options, "protocol_whitelist", "crypto,data,file", 0);
    }

    int result = avformat_open_input(&format, path, nullptr, &options);
    av_dict_free(&options);
    if (result >= 0) {
        result = avformat_find_stream_info(format, nullptr);
    }
    app.source_open_deadline_ms.store(0, std::memory_order_relaxed);
    if (result < 0) {
        error = "Open media source: " + ffmpeg_error(result);
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "source-open failed path=%s ffmpeg=%d error=%s",
            bfplayer::redact_uri_secrets(path).c_str(),
            result,
            error.c_str());
        avformat_close_input(&format);
        free_custom_avio(custom_io);
        app.local_media.close();
        return nullptr;
    }

    auto* source = static_cast<Kit_Source*>(std::calloc(1, sizeof(Kit_Source)));
    if (!source) {
        error = "Unable to allocate Kitchensink source";
        avformat_close_input(&format);
        free_custom_avio(custom_io);
        app.local_media.close();
        return nullptr;
    }
    source->format_ctx = format;
    source->avio_ctx = custom_io;
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "source-open success path=%s streams=%u duration_us=%lld format=%s",
        bfplayer::redact_uri_secrets(path).c_str(),
        format->nb_streams,
        static_cast<long long>(format->duration),
        format->iformat && format->iformat->name ? format->iformat->name : "<unknown>");
    return source;
}

std::string track_description(const App& app, int index) {
    if (!app.source || index < 0) {
        return "Off";
    }
    const auto* format = static_cast<const AVFormatContext*>(app.source->format_ctx);
    if (!format || index >= static_cast<int>(format->nb_streams)) {
        return "Track " + std::to_string(index);
    }
    const AVStream* stream = format->streams[index];
    std::string description;
    if (const AVDictionaryEntry* language =
            av_dict_get(stream->metadata, "language", nullptr, 0)) {
        description = bounded_metadata(language->value, 64);
    }
    if (const AVDictionaryEntry* title =
            av_dict_get(stream->metadata, "title", nullptr, 0)) {
        if (!description.empty()) {
            description += " - ";
        }
        description += bounded_metadata(title->value, 256);
    }
    const char* codec = avcodec_get_name(stream->codecpar->codec_id);
    if (codec && codec[0]) {
        if (!description.empty()) {
            description += " - ";
        }
        description += codec;
    }
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        stream->codecpar->ch_layout.nb_channels > 0) {
        description += " - " + std::to_string(stream->codecpar->ch_layout.nb_channels) + "ch";
    }
    return description.empty() ? "Track " + std::to_string(index) : description;
}

const AVStream* source_stream(const App& app, int index) {
    if (!app.source || index < 0) {
        return nullptr;
    }
    const auto* format = static_cast<const AVFormatContext*>(app.source->format_ctx);
    if (!format || index >= static_cast<int>(format->nb_streams)) {
        return nullptr;
    }
    return format->streams[index];
}

std::string stream_language(const App& app, int index) {
    const AVStream* stream = source_stream(app, index);
    if (!stream) {
        return {};
    }
    const AVDictionaryEntry* language =
        av_dict_get(stream->metadata, "language", nullptr, 0);
    return language && language->value
        ? bounded_metadata(language->value, 64)
        : std::string{};
}

AVMediaType media_type_for(Kit_StreamType type) {
    switch (type) {
        case KIT_STREAMTYPE_VIDEO:
            return AVMEDIA_TYPE_VIDEO;
        case KIT_STREAMTYPE_AUDIO:
            return AVMEDIA_TYPE_AUDIO;
        case KIT_STREAMTYPE_SUBTITLE:
            return AVMEDIA_TYPE_SUBTITLE;
        default:
            return AVMEDIA_TYPE_UNKNOWN;
    }
}

int resolve_preferred_stream(
    const App& app,
    Kit_StreamType type,
    int saved_index,
    const std::string& saved_language) {
    const AVMediaType wanted_type = media_type_for(type);
    const AVStream* saved_stream = source_stream(app, saved_index);
    if (saved_stream && saved_stream->codecpar->codec_type == wanted_type &&
        (saved_language.empty() || stream_language(app, saved_index) == saved_language)) {
        return saved_index;
    }
    if (saved_language.empty()) {
        return -1;
    }
    int index = Kit_GetNextSourceStream(app.source, type, -1, 0);
    while (index >= 0) {
        if (stream_language(app, index) == saved_language) {
            return index;
        }
        index = Kit_GetNextSourceStream(app.source, type, index, 0);
    }
    return -1;
}

SDL_Renderer* create_renderer(SDL_Window* window) {
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(
            window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC);
    }
    return renderer;
}

bool find_exact_display_mode(
    int width,
    int height,
    SDL_DisplayMode& selected) {
    const int count = SDL_GetNumDisplayModes(0);
    bool found = false;
    for (int index = 0; index < count; ++index) {
        SDL_DisplayMode candidate{};
        if (SDL_GetDisplayMode(0, index, &candidate) != 0 ||
            candidate.w != width ||
            candidate.h != height) {
            continue;
        }
        if (!found || candidate.refresh_rate > selected.refresh_rate) {
            selected = candidate;
            found = true;
        }
    }
    return found;
}

bool configure_fullscreen_output(App& app) {
    SDL_DisplayMode fallback{};
    if (!find_exact_display_mode(
            kWindowWidth,
            kWindowHeight,
            fallback)) {
        fallback.format = SDL_PIXELFORMAT_ABGR8888;
        fallback.w = kWindowWidth;
        fallback.h = kWindowHeight;
        fallback.refresh_rate = kPreferredOutputRefresh;
    }
    SDL_SetWindowSize(app.window, kWindowWidth, kWindowHeight);
    if (SDL_SetWindowDisplayMode(app.window, &fallback) != 0) {
        log_sdl("SDL_SetWindowDisplayMode(1080p)");
        return false;
    }
    if (SDL_SetWindowFullscreen(
            app.window,
            SDL_WINDOW_FULLSCREEN) != 0) {
        log_sdl("SDL_SetWindowFullscreen(1080p)");
        return false;
    }
    return true;
}

void log_display_output(App& app) {
    int window_width = 0;
    int window_height = 0;
    int renderer_width = 0;
    int renderer_height = 0;
    SDL_GetWindowSize(app.window, &window_width, &window_height);
    if (SDL_GetRendererOutputSize(
            app.renderer,
            &renderer_width,
            &renderer_height) != 0) {
        log_sdl("SDL_GetRendererOutputSize");
        renderer_width = window_width;
        renderer_height = window_height;
    }
    SDL_DisplayMode current{};
    if (SDL_GetCurrentDisplayMode(0, &current) != 0) {
        log_sdl("SDL_GetCurrentDisplayMode");
    }
    SDL_RendererInfo renderer_info{};
    if (SDL_GetRendererInfo(app.renderer, &renderer_info) != 0) {
        log_sdl("SDL_GetRendererInfo");
    }
    app.output_width = renderer_width > 0
        ? renderer_width
        : kWindowWidth;
    app.output_height = renderer_height > 0
        ? renderer_height
        : kWindowHeight;
    app.true_4k_output =
        app.output_width == kPreferredOutputWidth &&
        app.output_height == kPreferredOutputHeight &&
        current.w == kPreferredOutputWidth &&
        current.h == kPreferredOutputHeight;
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "display-output current=%dx%d@%d window=%dx%d renderer=%dx%d logical=%dx%d true_4k=%d renderer_name=%s flags=0x%x",
        current.w,
        current.h,
        current.refresh_rate,
        window_width,
        window_height,
        renderer_width,
        renderer_height,
        kWindowWidth,
        kWindowHeight,
        app.true_4k_output ? 1 : 0,
        renderer_info.name ? renderer_info.name : "unknown",
        renderer_info.flags);
}

bool switch_fullscreen_output(
    App& app,
    int width,
    int height,
    const char* reason) {
    if (!app.window || !app.renderer || width <= 0 || height <= 0) {
        return false;
    }
    if (app.output_width == width && app.output_height == height) {
        return true;
    }

    SDL_DisplayMode mode{};
    if (!find_exact_display_mode(width, height, mode)) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::warning,
            "display-switch unavailable requested=%dx%d reason=%s",
            width,
            height,
            reason ? reason : "<none>");
        return false;
    }
    if (SDL_SetWindowDisplayMode(app.window, &mode) != 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "display-switch failed requested=%dx%d reason=%s error=%s",
            width,
            height,
            reason ? reason : "<none>",
            SDL_GetError());
        return false;
    }
    SDL_PumpEvents();
    if (SDL_RenderSetLogicalSize(
            app.renderer,
            kWindowWidth,
            kWindowHeight) != 0) {
        log_sdl("SDL_RenderSetLogicalSize(display switch)");
        return false;
    }
    log_display_output(app);
    const bool matched =
        app.output_width == width && app.output_height == height;
    bfplayer::diagnostics_log(
        matched
            ? bfplayer::DiagnosticLevel::info
            : bfplayer::DiagnosticLevel::warning,
        "display-switch requested=%dx%d actual=%dx%d reason=%s matched=%d",
        width,
        height,
        app.output_width,
        app.output_height,
        reason ? reason : "<none>",
        matched ? 1 : 0);
    return matched;
}

void close_audio(App& app) {
    if (app.audio != 0) {
        SDL_ClearQueuedAudio(app.audio);
        SDL_CloseAudioDevice(app.audio);
        app.audio = 0;
    }
}

bool open_audio(App& app) {
    close_audio(app);
    if (Kit_GetPlayerAudioStream(app.player) < 0) {
        return true;
    }

    Kit_PlayerInfo info{};
    Kit_GetPlayerInfo(app.player, &info);
    SDL_AudioSpec wanted{};
    SDL_AudioSpec obtained{};
    wanted.freq = info.audio_format.sample_rate;
    wanted.format = static_cast<SDL_AudioFormat>(info.audio_format.format);
    wanted.channels = static_cast<Uint8>(info.audio_format.channels);
    wanted.samples = 1024;
    app.audio = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
    if (app.audio == 0) {
        log_sdl("SDL_OpenAudioDevice");
        return false;
    }
    SDL_PauseAudioDevice(app.audio, 0);
    return true;
}

void destroy_video_textures(App& app) {
    SDL_DestroyTexture(app.subtitles);
    SDL_DestroyTexture(app.video);
    app.subtitles = nullptr;
    app.video = nullptr;
}

bool create_subtitle_texture(App& app) {
    SDL_DestroyTexture(app.subtitles);
    app.subtitles = nullptr;
    if (Kit_GetPlayerSubtitleStream(app.player) < 0) {
        return true;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    app.subtitles = SDL_CreateTexture(
        app.renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STATIC,
        kSubtitleAtlasSize,
        kSubtitleAtlasSize);
    if (!app.subtitles) {
        log_sdl("SDL_CreateTexture(subtitle atlas)");
        return false;
    }
    SDL_SetTextureBlendMode(app.subtitles, SDL_BLENDMODE_BLEND);
    return true;
}

bool create_video_textures(App& app) {
    destroy_video_textures(app);
    Kit_PlayerInfo info{};
    Kit_GetPlayerInfo(app.player, &info);

    if (Kit_GetPlayerVideoStream(app.player) >= 0) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        app.video = SDL_CreateTexture(
            app.renderer,
            info.video_format.format,
            SDL_TEXTUREACCESS_STATIC,
            info.video_format.width,
            info.video_format.height);
        if (!app.video) {
            log_sdl("SDL_CreateTexture(video)");
            return false;
        }
        SDL_RenderSetLogicalSize(app.renderer, kWindowWidth, kWindowHeight);
    }

    return create_subtitle_texture(app);
}

double player_display_aspect(
    App& app,
    int frame_width,
    int frame_height) {
    // Prefer the demuxer's source dimensions and sample-aspect metadata. The
    // decoder output may be proportionally downscaled for performance, and
    // its frame metadata is not guaranteed to survive that conversion.
    if (app.source_display_aspect > 0.0 &&
        std::isfinite(app.source_display_aspect)) {
        return app.source_display_aspect;
    }
    int numerator = 0;
    int denominator = 0;
    Kit_GetPlayerAspectRatio(app.player, &numerator, &denominator);
    return bfplayer::display_aspect_from_sample_aspect(
        frame_width,
        frame_height,
        numerator,
        denominator);
}

bfplayer::VideoLayout player_video_layout(App& app) {
    Kit_PlayerInfo info{};
    Kit_GetPlayerInfo(app.player, &info);
    return bfplayer::compute_video_layout(
        info.video_format.width,
        info.video_format.height,
        player_display_aspect(
            app,
            info.video_format.width,
            info.video_format.height),
        kWindowWidth,
        kWindowHeight,
        app.video_scale_mode,
        app.video_aspect_mode,
        app.video_crop_mode);
}

bool set_stream(App& app, Kit_StreamType type, int index) {
    const int previous = Kit_GetPlayerStream(app.player, type);
    const double refresh_position = Kit_GetPlayerPosition(app.player);
    if (previous == index) {
        return true;
    }
    if (Kit_SetPlayerStream(app.player, type, index) != 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "track-switch failed type=%d previous=%d requested=%d error=%s",
            type,
            previous,
            index,
            Kit_GetError());
        return false;
    }
    bool resources_ready = true;
    if (type == KIT_STREAMTYPE_AUDIO) {
        resources_ready = open_audio(app);
    } else if (type == KIT_STREAMTYPE_VIDEO) {
        resources_ready = create_video_textures(app);
    } else if (type == KIT_STREAMTYPE_SUBTITLE) {
        resources_ready = create_subtitle_texture(app);
    }
    if (!resources_ready) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "track-switch resource-failure type=%d requested=%d restoring=%d",
            type,
            index,
            previous);
        const bool decoder_restored =
            Kit_SetPlayerStream(app.player, type, previous) == 0;
        const bool resources_restored = decoder_restored &&
            (type == KIT_STREAMTYPE_AUDIO
                 ? open_audio(app)
                 : type == KIT_STREAMTYPE_SUBTITLE
                     ? create_subtitle_texture(app)
                     : create_video_textures(app));
        if (!resources_restored) {
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::error,
                "track-switch rollback-failed type=%d error=%s",
                type,
                Kit_GetError());
            app.osd.show("Track switch failed; returning to library", 5000);
            app.playback_running = false;
        } else {
            app.osd.show("Track switch failed; previous track restored", 4000);
        }
        return false;
    }
    if (type == KIT_STREAMTYPE_VIDEO) {
        app.source_display_aspect = inspect_video_source(
            static_cast<const AVFormatContext*>(app.source->format_ctx),
            index).display_aspect;
    }
    if (index >= 0 &&
        (type == KIT_STREAMTYPE_AUDIO || type == KIT_STREAMTYPE_SUBTITLE) &&
        std::isfinite(refresh_position)) {
        /*
         * The demuxer may already be several seconds ahead of the playback
         * clock. Refresh packets around the current scene so the new track is
         * visible/audible immediately without changing the viewing position.
         */
        const double refresh_from = std::max(0.0, refresh_position);
        if (Kit_PlayerSeek(app.player, refresh_from) == 0) {
            if (app.audio != 0) {
                SDL_ClearQueuedAudio(app.audio);
            }
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::info,
                "track-switch refresh type=%d position=%.3f seek=%.3f",
                type,
                refresh_position,
                refresh_from);
        }
    }

    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "track-switch success type=%d previous=%d current=%d description=%s",
        type,
        previous,
        index,
        track_description(app, index).c_str());
    const char* type_name = type == KIT_STREAMTYPE_AUDIO ? "Audio" :
        (type == KIT_STREAMTYPE_VIDEO ? "Video" : "Subtitles");
    app.osd.show(std::string(type_name) + ": " + track_description(app, index));
    return true;
}

void cycle_stream(App& app, Kit_StreamType type, bool allow_off) {
    const int current = Kit_GetPlayerStream(app.player, type);
    int next = Kit_GetNextSourceStream(app.source, type, current, 0);
    if (next < 0 && allow_off && current >= 0) {
        set_stream(app, type, -1);
        return;
    }
    if (next < 0) {
        next = Kit_GetNextSourceStream(app.source, type, -1, 0);
    }
    if (next >= 0 && next != current) {
        set_stream(app, type, next);
    }
}

bool open_external_subtitle(App& app, int index) {
    if (index < 0 || index >= static_cast<int>(app.subtitle_sidecars.size())) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::warning,
            "subtitle-open rejected index=%d count=%zu",
            index,
            app.subtitle_sidecars.size());
        return false;
    }
    const int previous_embedded = Kit_GetPlayerSubtitleStream(app.player);
    if (!app.external_subtitles.open(
            app.renderer,
            app.subtitle_sidecars[static_cast<std::size_t>(index)],
            app.fallback_font,
            kWindowWidth,
            kWindowHeight)) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "subtitle-open failed path=%s error=%s",
            bfplayer::redact_uri_secrets(
                app.subtitle_sidecars[static_cast<std::size_t>(index)]).c_str(),
            app.external_subtitles.error().c_str());
        return false;
    }
    if (previous_embedded >= 0 &&
        !set_stream(app, KIT_STREAMTYPE_SUBTITLE, -1)) {
        app.external_subtitles.close();
        return false;
    }
    app.external_subtitle_index = index;
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "subtitle-open success index=%d path=%s",
        index,
        bfplayer::redact_uri_secrets(
            app.subtitle_sidecars[static_cast<std::size_t>(index)]).c_str());
    app.osd.show(
        "Subtitles: " + bfplayer::redact_uri_secrets(
            app.subtitle_sidecars[static_cast<std::size_t>(index)]));
    return true;
}

bfplayer::TrackPreferences current_track_preferences(const App& app) {
    bfplayer::TrackPreferences preferences{};
    if (!app.player) {
        return preferences;
    }
    preferences.audio_stream = Kit_GetPlayerAudioStream(app.player);
    preferences.audio_language = stream_language(app, preferences.audio_stream);
    if (app.external_subtitles.is_open() && app.external_subtitle_index >= 0 &&
        app.external_subtitle_index < static_cast<int>(app.subtitle_sidecars.size())) {
        preferences.external_subtitle = app.subtitle_sidecars[
            static_cast<std::size_t>(app.external_subtitle_index)];
    } else {
        preferences.subtitle_stream = Kit_GetPlayerSubtitleStream(app.player);
        preferences.subtitle_language = stream_language(app, preferences.subtitle_stream);
    }
    preferences.subtitle_delay_ms = app.subtitle_delay_ms;
    return preferences;
}

void restore_track_preferences(
    App& app,
    const bfplayer::TrackPreferences& preferences,
    bool explicit_subtitle) {
    app.subtitle_delay_ms = std::clamp<std::int64_t>(
        preferences.subtitle_delay_ms, -10000, 10000);

    const int audio_stream = resolve_preferred_stream(
        app,
        KIT_STREAMTYPE_AUDIO,
        preferences.audio_stream,
        preferences.audio_language);
    if (audio_stream >= 0 && audio_stream != Kit_GetPlayerAudioStream(app.player)) {
        set_stream(app, KIT_STREAMTYPE_AUDIO, audio_stream);
    }

    if (explicit_subtitle) {
        return;
    }
    if (!preferences.external_subtitle.empty()) {
        auto found = std::find(
            app.subtitle_sidecars.begin(),
            app.subtitle_sidecars.end(),
            preferences.external_subtitle);
        if (found == app.subtitle_sidecars.end()) {
            app.subtitle_sidecars.insert(
                app.subtitle_sidecars.begin(), preferences.external_subtitle);
            found = app.subtitle_sidecars.begin();
        }
        const int index = static_cast<int>(
            std::distance(app.subtitle_sidecars.begin(), found));
        if (open_external_subtitle(app, index)) {
            return;
        }
    }

    const int subtitle_stream = resolve_preferred_stream(
        app,
        KIT_STREAMTYPE_SUBTITLE,
        preferences.subtitle_stream,
        preferences.subtitle_language);
    if (subtitle_stream >= 0) {
        if (subtitle_stream != Kit_GetPlayerSubtitleStream(app.player)) {
            set_stream(app, KIT_STREAMTYPE_SUBTITLE, subtitle_stream);
        }
    } else if (Kit_GetPlayerSubtitleStream(app.player) >= 0) {
        set_stream(app, KIT_STREAMTYPE_SUBTITLE, -1);
    }
}

void cycle_subtitles(App& app) {
    if (app.external_subtitles.is_open()) {
        const int start = app.external_subtitle_index + 1;
        for (int index = start; index < static_cast<int>(app.subtitle_sidecars.size()); ++index) {
            if (open_external_subtitle(app, index)) {
                return;
            }
        }
        app.external_subtitles.close();
        app.external_subtitle_index = -1;
        bfplayer::diagnostics_log(bfplayer::DiagnosticLevel::info, "subtitle-selection off");
        app.osd.show("Subtitles: Off");
        return;
    }

    const int current = Kit_GetPlayerSubtitleStream(app.player);
    const int next = Kit_GetNextSourceStream(
        app.source, KIT_STREAMTYPE_SUBTITLE, current, 0);
    if (next >= 0) {
        set_stream(app, KIT_STREAMTYPE_SUBTITLE, next);
        return;
    }

    if (current >= 0) {
        set_stream(app, KIT_STREAMTYPE_SUBTITLE, -1);
    }
    for (int index = 0; index < static_cast<int>(app.subtitle_sidecars.size()); ++index) {
        if (open_external_subtitle(app, index)) {
            return;
        }
    }

    if (current < 0) {
        const int first = Kit_GetNextSourceStream(
            app.source, KIT_STREAMTYPE_SUBTITLE, -1, 0);
        if (first >= 0) {
            set_stream(app, KIT_STREAMTYPE_SUBTITLE, first);
        }
    }
}

void seek_relative(App& app, double seconds) {
    const double duration = Kit_GetPlayerDuration(app.player);
    const double position = Kit_GetPlayerPosition(app.player);
    if (!std::isfinite(duration) || duration <= 0.0 ||
        !std::isfinite(position)) {
        app.osd.show("Seeking is unavailable for this live/unknown-duration source");
        return;
    }
    const double target = std::clamp(
        position + seconds, 0.0, duration);
    if (Kit_PlayerSeek(app.player, target) != 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "seek failed from=%.3f delta=%.3f target=%.3f error=%s",
            position,
            seconds,
            target,
            Kit_GetError());
    } else if (app.audio != 0) {
        SDL_ClearQueuedAudio(app.audio);
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "seek success from=%.3f delta=%.3f target=%.3f",
            position,
            seconds,
            target);
    }
    app.osd.show(
        std::string(seconds >= 0.0 ? "Seek +" : "Seek ") +
        std::to_string(static_cast<int>(seconds)) + " seconds");
}

void seek_chapter(App& app, int direction) {
    const auto* format = static_cast<const AVFormatContext*>(app.source->format_ctx);
    if (!format || format->nb_chapters == 0) {
        seek_relative(app, direction < 0 ? -600.0 : 600.0);
        return;
    }
    const double position = Kit_GetPlayerPosition(app.player);
    if (!std::isfinite(position)) {
        app.osd.show("Chapter navigation is unavailable");
        return;
    }
    int target_index = -1;
    if (direction > 0) {
        for (unsigned int index = 0; index < format->nb_chapters; ++index) {
            const AVChapter* chapter = format->chapters[index];
            const double start = chapter->start * av_q2d(chapter->time_base);
            if (start > position + 1.0) {
                target_index = static_cast<int>(index);
                break;
            }
        }
    } else {
        for (unsigned int index = 0; index < format->nb_chapters; ++index) {
            const AVChapter* chapter = format->chapters[index];
            const double start = chapter->start * av_q2d(chapter->time_base);
            if (start < position - 5.0) {
                target_index = static_cast<int>(index);
            } else {
                break;
            }
        }
        if (target_index < 0 && format->nb_chapters > 0) {
            target_index = 0;
        }
    }
    if (target_index < 0 || target_index >= static_cast<int>(format->nb_chapters)) {
        app.osd.show(direction > 0 ? "Last chapter" : "First chapter");
        return;
    }
    const AVChapter* chapter = format->chapters[target_index];
    const double target = chapter->start * av_q2d(chapter->time_base);
    if (Kit_PlayerSeek(app.player, target) == 0) {
        if (app.audio != 0) {
            SDL_ClearQueuedAudio(app.audio);
        }
        std::string label = "Chapter " + std::to_string(target_index + 1);
        if (const AVDictionaryEntry* title =
                av_dict_get(chapter->metadata, "title", nullptr, 0)) {
            label += ": ";
            label += title->value;
        }
        app.osd.show(std::move(label));
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "chapter-seek success index=%d target=%.3f",
            target_index,
            target);
    } else {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "chapter-seek failed index=%d target=%.3f error=%s",
            target_index,
            target,
            Kit_GetError());
    }
}

void set_volume(App& app, int percent) {
    app.volume_percent = std::clamp(percent, 0, 100);
    if (app.volume_percent > 0) {
        app.previous_volume_percent = app.volume_percent;
    }
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "volume percent=%d",
        app.volume_percent);
    app.osd.show("Volume: " + std::to_string(app.volume_percent) + "%");
}

void toggle_mute(App& app) {
    if (app.volume_percent == 0) {
        set_volume(app, app.previous_volume_percent);
    } else {
        app.previous_volume_percent = app.volume_percent;
        set_volume(app, 0);
    }
}

void adjust_subtitle_delay(App& app, std::int64_t delta_ms) {
    app.subtitle_delay_ms = std::clamp<std::int64_t>(
        app.subtitle_delay_ms + delta_ms, -10000, 10000);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "subtitle-delay delta_ms=%lld value_ms=%lld",
        static_cast<long long>(delta_ms),
        static_cast<long long>(app.subtitle_delay_ms));
    app.osd.show(
        "Subtitle timing: " +
            std::to_string(app.subtitle_delay_ms) +
            " ms (positive = later)",
        5000);
}

void toggle_pause(App& app) {
    const bool pause = !app.paused;
    app.paused = pause;
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "pause state=%s position=%.3f",
        app.paused ? "paused" : "playing",
        app.player ? Kit_GetPlayerPosition(app.player) : 0.0);
    if (app.paused) {
        Kit_PlayerPause(app.player);
        if (app.audio != 0) {
            SDL_PauseAudioDevice(app.audio, 1);
        }
    } else {
        Kit_PlayerPlay(app.player);
        if (app.audio != 0) {
            SDL_PauseAudioDevice(app.audio, 0);
        }
    }
    app.osd.show(app.paused ? "Paused" : "Playing");
    app.redraw_requested = true;
}

void set_pause_state(App& app, bool pause) {
    if (!app.player || app.paused == pause) {
        return;
    }
    toggle_pause(app);
}

void seek_absolute(App& app, double target) {
    if (!app.player) {
        return;
    }
    const double duration = Kit_GetPlayerDuration(app.player);
    const double position = Kit_GetPlayerPosition(app.player);
    if (!std::isfinite(duration) || duration <= 0.0 ||
        !std::isfinite(position) || !std::isfinite(target)) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::warning,
            "remote-seek rejected position=%.3f duration=%.3f target=%.3f",
            position,
            duration,
            target);
        return;
    }
    target = std::clamp(target, 0.0, duration);
    if (Kit_PlayerSeek(app.player, target) != 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "remote-seek failed from=%.3f target=%.3f error=%s",
            position,
            target,
            Kit_GetError());
        return;
    }
    if (app.audio != 0) {
        SDL_ClearQueuedAudio(app.audio);
    }
    app.redraw_requested = true;
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "remote-seek success from=%.3f target=%.3f",
        position,
        target);
}

void consume_remote_commands(App& app, bool in_playback) {
    bfplayer::RemoteCommand command;
    while (app.remote_control.poll(command)) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "remote-command apply sequence=%llu command=%d in_playback=%d",
            static_cast<unsigned long long>(command.sequence),
            static_cast<int>(command.type),
            in_playback ? 1 : 0);
        switch (command.type) {
            case bfplayer::RemoteCommandType::open:
                app.pending_remote_media_path = std::move(command.path);
                if (in_playback) {
                    app.playback_running = false;
                }
                break;
            case bfplayer::RemoteCommandType::play:
                if (in_playback) {
                    set_pause_state(app, false);
                }
                break;
            case bfplayer::RemoteCommandType::pause:
                if (in_playback) {
                    set_pause_state(app, true);
                }
                break;
            case bfplayer::RemoteCommandType::toggle_pause:
                if (in_playback) {
                    toggle_pause(app);
                }
                break;
            case bfplayer::RemoteCommandType::seek_relative:
                if (in_playback) {
                    seek_relative(app, command.value);
                    app.redraw_requested = true;
                }
                break;
            case bfplayer::RemoteCommandType::seek_absolute:
                if (in_playback) {
                    seek_absolute(app, command.value);
                }
                break;
            case bfplayer::RemoteCommandType::stop:
                if (in_playback) {
                    app.playback_running = false;
                }
                break;
            case bfplayer::RemoteCommandType::exit:
                app.playback_running = false;
                app.running = false;
                break;
        }
    }
}

void cycle_video_scale(App& app) {
    app.video_scale_mode = bfplayer::next_video_scale_mode(app.video_scale_mode);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "video-scale mode=%s",
        bfplayer::video_scale_mode_name(app.video_scale_mode));
    app.osd.show(
        std::string("Video scale: ") +
        bfplayer::video_scale_mode_name(app.video_scale_mode));
}

void cycle_video_aspect(App& app) {
    app.video_aspect_mode =
        bfplayer::next_video_aspect_mode(app.video_aspect_mode);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "video-aspect mode=%s",
        bfplayer::video_aspect_mode_name(app.video_aspect_mode));
    app.osd.show(
        std::string("Aspect ratio: ") +
        bfplayer::video_aspect_mode_name(app.video_aspect_mode));
}

void cycle_video_crop(App& app) {
    app.video_crop_mode =
        bfplayer::next_video_crop_mode(app.video_crop_mode);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "video-crop mode=%s",
        bfplayer::video_crop_mode_name(app.video_crop_mode));
    app.osd.show(
        std::string("Crop ratio: ") +
        bfplayer::video_crop_mode_name(app.video_crop_mode));
}

bool persist_active_player_settings(App& app) {
    const bfplayer::PlayerSettings settings =
        bfplayer::normalized_player_settings(app.settings);
    bfplayer::LibraryDatabase database;
    const bool saved =
        database.open("/data/BFplayer/library.db") &&
        database.set_settings({
            {std::string(bfplayer::kSettingVolumePercent),
             std::to_string(settings.volume_percent)},
            {std::string(bfplayer::kSettingShortSeekSeconds),
             std::to_string(settings.short_seek_seconds)},
            {std::string(bfplayer::kSettingLongSeekSeconds),
             std::to_string(settings.long_seek_seconds)},
            {std::string(bfplayer::kSettingOsdDurationMs),
             std::to_string(settings.osd_duration_ms)},
            {std::string(bfplayer::kSettingResumePlayback),
             settings.resume_playback ? "1" : "0"},
            {std::string(bfplayer::kSettingAutoSubtitles),
             settings.auto_subtitles ? "1" : "0"},
            {std::string(bfplayer::kSettingSubdlApiKey),
             settings.subdl_api_key},
            {std::string(bfplayer::kSettingSubtitleLanguages),
             settings.subtitle_languages},
        });
    if (!saved) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "player-settings live-save failed error=%s",
            database.error().c_str());
        return false;
    }
    app.settings = settings;
    app.volume_percent = settings.volume_percent;
    if (app.volume_percent > 0) {
        app.previous_volume_percent = app.volume_percent;
    }
    app.osd.set_default_duration(
        static_cast<std::uint64_t>(settings.osd_duration_ms));
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "player-settings live-save volume=%d short_seek=%d long_seek=%d osd_ms=%d resume=%d auto_subtitles=%d subdl_key=%d subtitle_languages=%s scale=%s aspect=%s crop=%s",
        settings.volume_percent,
        settings.short_seek_seconds,
        settings.long_seek_seconds,
        settings.osd_duration_ms,
        settings.resume_playback ? 1 : 0,
        settings.auto_subtitles ? 1 : 0,
        settings.subdl_api_key.empty() ? 0 : 1,
        settings.subtitle_languages.c_str(),
        bfplayer::video_scale_mode_name(app.video_scale_mode),
        bfplayer::video_aspect_mode_name(app.video_aspect_mode),
        bfplayer::video_crop_mode_name(app.video_crop_mode));
    return true;
}

std::string path_filename(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos
        ? path
        : path.substr(slash + 1);
}

std::vector<SubtitleMenuItem> subtitle_menu_items(const App& app) {
    std::vector<SubtitleMenuItem> items;
    items.push_back({SubtitleMenuKind::off, -1, "Off"});
    int stream = Kit_GetNextSourceStream(
        app.source,
        KIT_STREAMTYPE_SUBTITLE,
        -1,
        0);
    while (stream >= 0 && items.size() < 64) {
        items.push_back({
            SubtitleMenuKind::embedded,
            stream,
            "Embedded  |  " +
                track_description(app, stream)});
        stream = Kit_GetNextSourceStream(
            app.source,
            KIT_STREAMTYPE_SUBTITLE,
            stream,
            0);
    }
    for (std::size_t index = 0;
         index < app.subtitle_sidecars.size() && items.size() < 96;
         ++index) {
        items.push_back({
            SubtitleMenuKind::external,
            static_cast<int>(index),
            "File  |  " +
                path_filename(app.subtitle_sidecars[index])});
    }
    items.push_back({
        SubtitleMenuKind::browse,
        -1,
        "Browse subtitle files..."});
    items.push_back({
        SubtitleMenuKind::providers,
        -1,
        "Online subtitle providers..."});
    return items;
}

std::string online_subtitle_label(
    const bfplayer::OnlineSubtitle& subtitle) {
    std::string label = subtitle.language.empty()
        ? "Unknown language"
        : subtitle.language;
    if (subtitle.hearing_impaired) {
        label += " CC";
    }
    if (!subtitle.fps.empty() && subtitle.fps != "0") {
        label += "  |  " + subtitle.fps + " fps";
    }
    label += "  |  " + subtitle.release_name;
    return label;
}

void refresh_subtitle_browser_overlay(App& app) {
    constexpr int kVisibleSubtitleRows = 8;
    const int count =
        static_cast<int>(app.subtitle_browser.entries.size());
    app.subtitle_browser_selected = std::clamp(
        app.subtitle_browser_selected,
        0,
        std::max(0, count - 1));
    app.subtitle_browser_first = std::clamp(
        app.subtitle_browser_first,
        std::max(
            0,
            app.subtitle_browser_selected -
                kVisibleSubtitleRows + 1),
        app.subtitle_browser_selected);
    app.subtitle_browser_first = std::min(
        app.subtitle_browser_first,
        std::max(0, count - kVisibleSubtitleRows));
    std::vector<std::string> rows;
    const int end = std::min(
        count,
        app.subtitle_browser_first + kVisibleSubtitleRows);
    for (int index = app.subtitle_browser_first; index < end; ++index) {
        const auto& entry =
            app.subtitle_browser.entries[static_cast<std::size_t>(index)];
        rows.push_back(
            entry.directory
                ? "[Folder]  " + entry.name
                : entry.name);
    }
    if (rows.empty()) {
        rows.push_back("No subtitle files in this folder");
    }
    app.osd.show_panel(
        "Choose subtitle file - Circle: parent - Options: cancel  |  " +
            app.subtitle_browser.path,
        std::move(rows),
        count == 0
            ? -1
            : app.subtitle_browser_selected -
                  app.subtitle_browser_first);
}

void refresh_text_edit_overlay(App& app) {
    const bool editing_key =
        app.text_edit_mode == TextEditMode::subdl_api_key;
    const bool editing_search =
        app.text_edit_mode == TextEditMode::subtitle_search_query;
    std::string shown;
    if (editing_key) {
        shown.assign(
            std::min<std::size_t>(app.text_edit_buffer.size(), 48),
            '*');
        if (shown.empty()) {
            shown = "(empty clears the key)";
        }
    } else {
        shown = app.text_edit_buffer.empty()
            ? (editing_search
                   ? "(example: One Piece S01E01)"
                   : "(example: en,ar,fr)")
            : app.text_edit_buffer;
    }
    app.osd.show_panel(
        editing_key
            ? "Enter your SubDL API key - Return: save - Circle: cancel"
            : (editing_search
                   ? "Search SubDL by title - Return: search - Circle: cancel"
                   : "Subtitle languages - comma-separated codes - Return: save"),
        {std::move(shown)},
        -1);
}

void refresh_playback_overlay(App& app) {
    if (app.playback_overlay == PlaybackOverlay::none) {
        app.osd.hide_panel();
        return;
    }
    if (app.text_edit_mode != TextEditMode::none) {
        refresh_text_edit_overlay(app);
        return;
    }
    if (app.playback_overlay == PlaybackOverlay::subtitle_browser) {
        refresh_subtitle_browser_overlay(app);
        return;
    }
    if (app.playback_overlay == PlaybackOverlay::subtitle_providers) {
        app.playback_overlay_selected = std::clamp(
            app.playback_overlay_selected,
            0,
            3);
        app.osd.show_panel(
            "Online subtitle providers - grey means unavailable",
            {
                app.settings.subdl_api_key.empty()
                    ? "SubDL  |  API key not set"
                    : "SubDL  |  Ready  |  " +
                          app.settings.subtitle_languages,
                "OpenSubtitles  |  Requires an approved app API key",
                "Podnapisi  |  No supported public API",
                "Addic7ed  |  No supported public API",
            },
            app.playback_overlay_selected,
            {true, false, false, false});
        return;
    }
    if (app.playback_overlay == PlaybackOverlay::subtitle_subdl) {
        app.playback_overlay_selected = std::clamp(
            app.playback_overlay_selected,
            0,
            2);
        app.osd.show_panel(
            "SubDL - Cross: select - Circle: providers",
            {
                "Search by video filename",
                "Search by movie or show title...",
                std::string("API key  |  ") +
                    (app.settings.subdl_api_key.empty()
                         ? "Not set"
                         : "Set"),
            },
            app.playback_overlay_selected);
        return;
    }
    if (app.playback_overlay == PlaybackOverlay::subtitle_online) {
        std::vector<std::string> rows;
        if (app.subtitle_job.thread &&
            !app.subtitle_job.done.load(std::memory_order_acquire)) {
            rows.push_back(
                app.subtitle_job.type == SubtitleJobType::search
                    ? "Searching SubDL..."
                    : "Downloading subtitle...");
            app.osd.show_panel(
                "Online subtitles",
                std::move(rows),
                -1);
            return;
        }
        if (!app.subtitle_online_error.empty()) {
            rows.push_back(app.subtitle_online_error);
            rows.push_back(
                "Use Search SubDL by title for custom release names");
            app.osd.show_panel(
                "SubDL request failed - Circle: back",
                std::move(rows),
                -1);
            return;
        }
        constexpr int kVisibleRows = 12;
        const int count =
            static_cast<int>(app.online_subtitles.size());
        app.playback_overlay_selected = std::clamp(
            app.playback_overlay_selected,
            0,
            std::max(0, count - 1));
        const int first = std::clamp(
            app.playback_overlay_selected - kVisibleRows + 1,
            0,
            std::max(0, count - kVisibleRows));
        const int end = std::min(count, first + kVisibleRows);
        for (int index = first; index < end; ++index) {
            rows.push_back(
                online_subtitle_label(
                    app.online_subtitles[
                        static_cast<std::size_t>(index)]));
        }
        if (rows.empty()) {
            rows.push_back("No results");
        }
        app.osd.show_panel(
            "SubDL results - Cross: download - Circle: back",
            std::move(rows),
            app.online_subtitles.empty()
                ? -1
                : app.playback_overlay_selected - first);
        return;
    }
    if (app.playback_overlay == PlaybackOverlay::subtitles) {
        const auto items = subtitle_menu_items(app);
        std::vector<std::string> rows;
        constexpr int kVisibleRows = 12;
        const int count = static_cast<int>(items.size());
        app.playback_overlay_selected = std::clamp(
            app.playback_overlay_selected,
            0,
            std::max(0, count - 1));
        const int first = std::clamp(
            app.playback_overlay_selected - kVisibleRows + 1,
            0,
            std::max(0, count - kVisibleRows));
        const int end = std::min(count, first + kVisibleRows);
        rows.reserve(static_cast<std::size_t>(end - first));
        for (int index = first; index < end; ++index) {
            rows.push_back(
                items[static_cast<std::size_t>(index)].label);
        }
        app.osd.show_panel(
            "Subtitles - Cross: select - Circle: back",
            std::move(rows),
            app.playback_overlay_selected - first);
        return;
    }
    if (app.playback_overlay == PlaybackOverlay::controls) {
        app.osd.show_panel(
            "Playback controls",
            {
                "Cross             Play or pause",
                "D-pad Left/Right  Seek short step",
                "D-pad Down/Up     Seek long step",
                "L1 / R1           Previous / next chapter",
                "Circle            Quick-change subtitle track",
                "Square            Change audio track",
                "Triangle          Change video track",
                "L3 / R3           Volume down / up",
                "L2 + Triangle     Crop mode",
                "R2 + Triangle     Aspect ratio",
                "L2+R2+Triangle    Scale mode",
                "Touchpad          Show this controls page",
                "Options           Menu, subtitles, settings, return",
            },
            -1);
        return;
    }
    if (app.playback_overlay == PlaybackOverlay::settings) {
        app.osd.show_panel(
            "Playback settings - Left/Right change | Square reset | Circle back",
            {
                "Volume                          " +
                    std::to_string(app.settings.volume_percent) + "%",
                "Short seek step                 " +
                    std::to_string(app.settings.short_seek_seconds) +
                    " seconds",
                "Long seek step                  " +
                    std::to_string(app.settings.long_seek_seconds) +
                    " seconds",
                "Pop-up message duration         " +
                    std::to_string(app.settings.osd_duration_ms / 1000) +
                    " seconds",
                std::string("Video scaling                   ") +
                    bfplayer::video_scale_mode_name(app.video_scale_mode),
                std::string("Display aspect (this video)     ") +
                    bfplayer::video_aspect_mode_name(app.video_aspect_mode),
                std::string("Crop                            ") +
                    bfplayer::video_crop_mode_name(app.video_crop_mode),
                std::string("Resume where I stopped         ") +
                    (app.settings.resume_playback ? "On" : "Off"),
                std::string("Automatically select subtitles  ") +
                    (app.settings.auto_subtitles ? "On" : "Off"),
                std::string("SubDL API key                   ") +
                    (app.settings.subdl_api_key.empty()
                         ? "Not set"
                         : "Set"),
                "Download languages               " +
                    app.settings.subtitle_languages,
                "Restore playback defaults",
            },
            app.playback_overlay_selected);
        return;
    }
    app.osd.show_panel(
        "Playback menu",
        {
            "Resume playback",
            "Subtitles",
            "View all controls",
            "Playback settings",
            app.volume_percent == 0 ? "Unmute audio" : "Mute audio",
            "Subtitle timing              " +
                std::to_string(app.subtitle_delay_ms) +
                " ms   (Left / Right)",
            "Return to library",
        },
        app.playback_overlay_selected);
}

void open_playback_overlay(App& app, PlaybackOverlay overlay) {
    app.playback_overlay = overlay;
    app.playback_overlay_selected = 0;
    refresh_playback_overlay(app);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-overlay open=%s",
        overlay == PlaybackOverlay::menu
            ? "menu"
            : (overlay == PlaybackOverlay::controls
                   ? "controls"
                   : (overlay == PlaybackOverlay::settings
                          ? "settings"
                          : (overlay == PlaybackOverlay::subtitles
                                 ? "subtitles"
                                 : (overlay ==
                                            PlaybackOverlay::subtitle_browser
                                        ? "subtitle-browser"
                                        : "subtitle-online")))));
}

void erase_last_utf8_codepoint(std::string& value) {
    if (value.empty()) {
        return;
    }
    value.pop_back();
    while (!value.empty() &&
           (static_cast<unsigned char>(value.back()) & 0xc0U) == 0x80U) {
        value.pop_back();
    }
}

void begin_text_edit(App& app, TextEditMode mode) {
    if (app.text_edit_mode != TextEditMode::none) {
        return;
    }
    app.text_edit_previous_settings = app.settings;
    app.text_edit_mode = mode;
    app.text_edit_buffer =
        mode == TextEditMode::subtitle_languages
        ? app.settings.subtitle_languages
        : std::string{};
    app.ime_was_visible = false;
    SDL_StartTextInput();
    refresh_playback_overlay(app);
}

void start_subtitle_search(
    App& app,
    const std::string& search_query = {});

void finish_text_edit(App& app, bool commit) {
    if (app.text_edit_mode == TextEditMode::none) {
        return;
    }
    const TextEditMode mode = app.text_edit_mode;
    app.text_edit_mode = TextEditMode::none;
    app.ime_was_visible = false;
    SDL_StopTextInput();
    if (!commit) {
        app.settings = app.text_edit_previous_settings;
        app.text_edit_buffer.clear();
        refresh_playback_overlay(app);
        return;
    }
    if (mode == TextEditMode::subtitle_search_query) {
        std::string query = app.text_edit_buffer;
        const std::size_t first = query.find_first_not_of(" \t\r\n");
        const std::size_t last = query.find_last_not_of(" \t\r\n");
        app.text_edit_buffer.clear();
        if (first == std::string::npos) {
            app.subtitle_online_error = "Enter a movie or show title";
            app.playback_overlay = PlaybackOverlay::subtitle_online;
            refresh_playback_overlay(app);
            return;
        }
        query = query.substr(first, last - first + 1);
        start_subtitle_search(app, query);
        return;
    }
    if (mode == TextEditMode::subdl_api_key) {
        app.settings.subdl_api_key = app.text_edit_buffer;
    } else {
        app.settings.subtitle_languages = app.text_edit_buffer;
    }
    app.text_edit_buffer.clear();
    if (!persist_active_player_settings(app)) {
        app.settings = app.text_edit_previous_settings;
        app.osd.show("Unable to save subtitle download settings", 8000);
    }
    refresh_playback_overlay(app);
}

bool load_subtitle_browser(App& app, const std::string& path) {
    bfplayer::SubtitleBrowserResult result =
        bfplayer::list_subtitle_directory(path);
    if (!result.ok()) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "subtitle-browser failed path=%s error=%s",
            path.c_str(),
            result.error.c_str());
        app.osd.show(result.error, 8000);
        return false;
    }
    app.subtitle_browser = std::move(result);
    app.subtitle_browser_selected = 0;
    app.subtitle_browser_first = 0;
    app.playback_overlay = PlaybackOverlay::subtitle_browser;
    refresh_playback_overlay(app);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "subtitle-browser opened path=%s shown=%zu entries_seen=%zu directories=%zu subtitles=%zu stat_fallbacks=%zu unreadable=%zu",
        app.subtitle_browser.path.c_str(),
        app.subtitle_browser.entries.size(),
        app.subtitle_browser.entries_seen,
        app.subtitle_browser.directories,
        app.subtitle_browser.subtitle_files,
        app.subtitle_browser.stat_fallbacks,
        app.subtitle_browser.unreadable_entries);
    return true;
}

void open_subtitle_browser(App& app) {
    std::string initial = "/";
    if (!bfplayer::is_network_uri(app.current_media_path)) {
        initial =
            bfplayer::subtitle_browser_parent(app.current_media_path);
    }
    if (!load_subtitle_browser(app, initial) && initial != "/") {
        (void)load_subtitle_browser(app, "/");
    }
}

int subtitle_job_entry(void* userdata) {
    auto* job = static_cast<SubtitleJob*>(userdata);
    if (!job) {
        return 1;
    }
    if (job->type == SubtitleJobType::search) {
        job->search =
            job->search_by_title
            ? bfplayer::search_subdl_title(
                  job->api_key,
                  job->media_path,
                  job->languages)
            : bfplayer::search_subdl(
                  job->api_key,
                  job->media_path,
                  job->languages);
        if (!job->search.ok()) {
            job->error = job->search.error;
        }
    } else if (job->type == SubtitleJobType::download) {
        job->download =
            bfplayer::download_subdl(job->api_key, job->selected);
        if (!job->download.ok()) {
            job->error = job->download.error;
        } else if (!bfplayer::save_downloaded_subtitle(
                       "/data/BFplayer/subtitles",
                       job->media_path,
                       job->selected,
                       job->download,
                       job->saved_path,
                       job->error)) {
            job->download.bytes.clear();
        }
    } else {
        job->error = "Invalid subtitle job";
    }
    job->done.store(true, std::memory_order_release);
    return job->error.empty() ? 0 : 1;
}

bool start_subtitle_job(
    App& app,
    SubtitleJobType type,
    const std::string& search_query = {}) {
    if (app.subtitle_job.thread) {
        app.osd.show("A subtitle request is already running");
        return false;
    }
    app.subtitle_job.done.store(false, std::memory_order_release);
    app.subtitle_job.type = type;
    app.subtitle_job.api_key = app.settings.subdl_api_key;
    app.subtitle_job.media_path =
        search_query.empty() ? app.current_media_path : search_query;
    app.subtitle_job.languages = app.settings.subtitle_languages;
    app.subtitle_job.search_by_title = !search_query.empty();
    app.subtitle_job.search = {};
    app.subtitle_job.download = {};
    app.subtitle_job.saved_path.clear();
    app.subtitle_job.error.clear();
    app.subtitle_online_error.clear();
    app.subtitle_job.thread = SDL_CreateThread(
        subtitle_job_entry,
        type == SubtitleJobType::search
            ? "bfplayer-subtitle-search"
            : "bfplayer-subtitle-download",
        &app.subtitle_job);
    if (!app.subtitle_job.thread) {
        app.subtitle_job.error =
            std::string("Unable to start subtitle request: ") +
            SDL_GetError();
        app.subtitle_job.type = SubtitleJobType::none;
        app.osd.show(app.subtitle_job.error, 8000);
        return false;
    }
    app.playback_overlay = PlaybackOverlay::subtitle_online;
    app.playback_overlay_selected = 0;
    refresh_playback_overlay(app);
    return true;
}

void start_subtitle_search(
    App& app,
    const std::string& search_query) {
    if (app.settings.subdl_api_key.empty()) {
        app.playback_overlay = PlaybackOverlay::settings;
        app.playback_overlay_selected = 9;
        app.osd.show(
            "Set your free SubDL API key, then try Download again",
            8000);
        refresh_playback_overlay(app);
        return;
    }
    app.online_subtitles.clear();
    if (start_subtitle_job(
            app,
            SubtitleJobType::search,
            search_query)) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "subtitle-provider search provider=subdl languages=%s mode=%s query=%s",
            app.settings.subtitle_languages.c_str(),
            search_query.empty() ? "filename" : "title",
            bfplayer::redact_uri_secrets(
                search_query.empty()
                    ? app.current_media_path
                    : search_query).c_str());
    }
}

void start_subtitle_download(
    App& app,
    const bfplayer::OnlineSubtitle& subtitle) {
    app.subtitle_job.selected = subtitle;
    if (start_subtitle_job(app, SubtitleJobType::download)) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "subtitle-provider download provider=subdl language=%s id=%s",
            subtitle.language.c_str(),
            subtitle.id.c_str());
    }
}

void consume_subtitle_job(App& app) {
    if (!app.subtitle_job.thread ||
        !app.subtitle_job.done.load(std::memory_order_acquire)) {
        return;
    }
    SDL_WaitThread(app.subtitle_job.thread, nullptr);
    app.subtitle_job.thread = nullptr;
    const SubtitleJobType completed = app.subtitle_job.type;
    app.subtitle_job.type = SubtitleJobType::none;
    if (!app.subtitle_job.error.empty()) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "subtitle-provider failed provider=subdl operation=%s error=%s",
            completed == SubtitleJobType::search ? "search" : "download",
            app.subtitle_job.error.c_str());
        const std::string error = app.subtitle_job.error;
        app.subtitle_job.error.clear();
        app.subtitle_online_error = error;
        app.playback_overlay = PlaybackOverlay::subtitle_online;
        if (completed == SubtitleJobType::search) {
            app.online_subtitles.clear();
        }
        refresh_playback_overlay(app);
        return;
    }
    if (completed == SubtitleJobType::search) {
        app.online_subtitles =
            std::move(app.subtitle_job.search.subtitles);
        app.subtitle_online_error.clear();
        app.playback_overlay = PlaybackOverlay::subtitle_online;
        app.playback_overlay_selected = 0;
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "subtitle-provider results provider=subdl count=%zu",
            app.online_subtitles.size());
        refresh_playback_overlay(app);
        return;
    }
    const std::string saved = app.subtitle_job.saved_path;
    if (!saved.empty()) {
        const auto existing = std::find(
            app.subtitle_sidecars.begin(),
            app.subtitle_sidecars.end(),
            saved);
        int index = 0;
        if (existing == app.subtitle_sidecars.end()) {
            app.subtitle_sidecars.push_back(saved);
            index =
                static_cast<int>(app.subtitle_sidecars.size() - 1);
        } else {
            index = static_cast<int>(
                std::distance(app.subtitle_sidecars.begin(), existing));
        }
        app.playback_overlay = PlaybackOverlay::none;
        refresh_playback_overlay(app);
        if (open_external_subtitle(app, index)) {
            app.osd.show(
                "Downloaded and selected: " +
                    path_filename(saved),
                8000);
        }
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "subtitle-provider installed provider=subdl path=%s",
            saved.c_str());
    }
}

void stop_subtitle_job(App& app) {
    if (app.subtitle_job.thread) {
        SDL_WaitThread(app.subtitle_job.thread, nullptr);
        app.subtitle_job.thread = nullptr;
    }
    app.subtitle_job.type = SubtitleJobType::none;
    app.subtitle_job.done.store(false, std::memory_order_release);
}

bool handle_playback_overlay_button(
    App& app,
    SDL_GameControllerButton button) {
    if (app.playback_overlay == PlaybackOverlay::none) {
        return false;
    }
    if (app.text_edit_mode != TextEditMode::none) {
        // The PS5 IME owns controller input until it closes.
        return true;
    }
    if (button == bfplayer::kControllerOptionsButton) {
        app.playback_overlay =
            app.playback_overlay == PlaybackOverlay::subtitle_browser
                ? PlaybackOverlay::subtitles
                : (app.playback_overlay ==
                           PlaybackOverlay::subtitle_providers
                       ? PlaybackOverlay::subtitles
                       : (app.playback_overlay ==
                                  PlaybackOverlay::subtitle_subdl
                              ? PlaybackOverlay::subtitle_providers
                              : PlaybackOverlay::none));
        refresh_playback_overlay(app);
        return true;
    }
    if (app.playback_overlay == PlaybackOverlay::subtitle_browser) {
        const int count =
            static_cast<int>(app.subtitle_browser.entries.size());
        if (button == SDL_CONTROLLER_BUTTON_B) {
            if (app.subtitle_browser.path == "/") {
                open_playback_overlay(app, PlaybackOverlay::subtitles);
            } else {
                (void)load_subtitle_browser(
                    app,
                    bfplayer::subtitle_browser_parent(
                        app.subtitle_browser.path));
            }
            return true;
        }
        int delta = 0;
        if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
            delta = -1;
        } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            delta = 1;
        } else if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
            delta = -8;
        } else if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
            delta = 8;
        }
        if (delta != 0) {
            app.subtitle_browser_selected =
                bfplayer::wrap_list_index(
                    app.subtitle_browser_selected,
                    delta,
                    count);
            refresh_playback_overlay(app);
            return true;
        }
        if (button == SDL_CONTROLLER_BUTTON_A && count > 0) {
            const auto selected =
                app.subtitle_browser.entries[
                    static_cast<std::size_t>(
                        app.subtitle_browser_selected)];
            if (selected.directory) {
                (void)load_subtitle_browser(app, selected.path);
            } else {
                const auto existing = std::find(
                    app.subtitle_sidecars.begin(),
                    app.subtitle_sidecars.end(),
                    selected.path);
                int index = 0;
                if (existing == app.subtitle_sidecars.end()) {
                    app.subtitle_sidecars.push_back(selected.path);
                    index = static_cast<int>(
                        app.subtitle_sidecars.size() - 1);
                } else {
                    index = static_cast<int>(
                        std::distance(
                            app.subtitle_sidecars.begin(),
                            existing));
                }
                app.playback_overlay = PlaybackOverlay::none;
                refresh_playback_overlay(app);
                (void)open_external_subtitle(app, index);
            }
        }
        return true;
    }
    if (app.playback_overlay == PlaybackOverlay::subtitle_providers) {
        if (button == SDL_CONTROLLER_BUTTON_B) {
            open_playback_overlay(app, PlaybackOverlay::subtitles);
            return true;
        }
        if (button == SDL_CONTROLLER_BUTTON_DPAD_UP ||
            button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            const int direction =
                button == SDL_CONTROLLER_BUTTON_DPAD_UP ? -1 : 1;
            app.playback_overlay_selected =
                bfplayer::wrap_list_index(
                    app.playback_overlay_selected,
                    direction,
                    4);
            refresh_playback_overlay(app);
            return true;
        }
        if (button == SDL_CONTROLLER_BUTTON_A) {
            if (app.playback_overlay_selected == 0) {
                open_playback_overlay(
                    app,
                    PlaybackOverlay::subtitle_subdl);
            } else if (app.playback_overlay_selected == 1) {
                app.osd.show(
                    "OpenSubtitles needs a registered application API key",
                    8000);
            } else {
                app.osd.show(
                    "This provider has no supported public API; website scraping is disabled",
                    8000);
            }
        }
        return true;
    }
    if (app.playback_overlay == PlaybackOverlay::subtitle_subdl) {
        if (button == SDL_CONTROLLER_BUTTON_B) {
            open_playback_overlay(
                app,
                PlaybackOverlay::subtitle_providers);
            return true;
        }
        if (button == SDL_CONTROLLER_BUTTON_DPAD_UP ||
            button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            const int direction =
                button == SDL_CONTROLLER_BUTTON_DPAD_UP ? -1 : 1;
            app.playback_overlay_selected =
                bfplayer::wrap_list_index(
                    app.playback_overlay_selected,
                    direction,
                    3);
            refresh_playback_overlay(app);
            return true;
        }
        if (button == SDL_CONTROLLER_BUTTON_A) {
            if (app.playback_overlay_selected == 0) {
                start_subtitle_search(app);
            } else if (app.playback_overlay_selected == 1) {
                if (app.settings.subdl_api_key.empty()) {
                    start_subtitle_search(app);
                } else {
                    begin_text_edit(
                        app,
                        TextEditMode::subtitle_search_query);
                }
            } else {
                app.playback_overlay =
                    PlaybackOverlay::settings;
                app.playback_overlay_selected = 9;
                refresh_playback_overlay(app);
            }
        }
        return true;
    }
    if (app.playback_overlay == PlaybackOverlay::subtitle_online) {
        if (app.subtitle_job.thread) {
            return true;
        }
        if (button == SDL_CONTROLLER_BUTTON_B) {
            open_playback_overlay(
                app,
                PlaybackOverlay::subtitle_subdl);
            return true;
        }
        const int count =
            static_cast<int>(app.online_subtitles.size());
        if (count > 0 &&
            (button == SDL_CONTROLLER_BUTTON_DPAD_UP ||
             button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
            const int direction =
                button == SDL_CONTROLLER_BUTTON_DPAD_UP ? -1 : 1;
            app.playback_overlay_selected =
                (app.playback_overlay_selected + count + direction) %
                count;
            refresh_playback_overlay(app);
            return true;
        }
        if (button == SDL_CONTROLLER_BUTTON_A && count > 0) {
            start_subtitle_download(
                app,
                app.online_subtitles[
                    static_cast<std::size_t>(
                        app.playback_overlay_selected)]);
        }
        return true;
    }
    if (app.playback_overlay == PlaybackOverlay::subtitles) {
        if (button == SDL_CONTROLLER_BUTTON_B) {
            open_playback_overlay(app, PlaybackOverlay::menu);
            return true;
        }
        const auto items = subtitle_menu_items(app);
        const int count = static_cast<int>(items.size());
        if (button == SDL_CONTROLLER_BUTTON_DPAD_UP ||
            button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            const int direction =
                button == SDL_CONTROLLER_BUTTON_DPAD_UP ? -1 : 1;
            app.playback_overlay_selected =
                (app.playback_overlay_selected + count + direction) %
                count;
            refresh_playback_overlay(app);
            return true;
        }
        if (button != SDL_CONTROLLER_BUTTON_A || count == 0) {
            return true;
        }
        const SubtitleMenuItem& selected =
            items[static_cast<std::size_t>(
                app.playback_overlay_selected)];
        switch (selected.kind) {
            case SubtitleMenuKind::off:
                app.external_subtitles.close();
                app.external_subtitle_index = -1;
                if (Kit_GetPlayerSubtitleStream(app.player) >= 0) {
                    (void)set_stream(
                        app,
                        KIT_STREAMTYPE_SUBTITLE,
                        -1);
                }
                app.playback_overlay = PlaybackOverlay::none;
                refresh_playback_overlay(app);
                app.osd.show("Subtitles: Off");
                break;
            case SubtitleMenuKind::embedded:
                app.external_subtitles.close();
                app.external_subtitle_index = -1;
                if (set_stream(
                        app,
                        KIT_STREAMTYPE_SUBTITLE,
                        selected.index)) {
                    app.playback_overlay = PlaybackOverlay::none;
                    refresh_playback_overlay(app);
                }
                break;
            case SubtitleMenuKind::external:
                if (open_external_subtitle(app, selected.index)) {
                    app.playback_overlay = PlaybackOverlay::none;
                    refresh_playback_overlay(app);
                }
                break;
            case SubtitleMenuKind::browse:
                open_subtitle_browser(app);
                break;
            case SubtitleMenuKind::providers:
                open_playback_overlay(
                    app,
                    PlaybackOverlay::subtitle_providers);
                break;
        }
        return true;
    }
    if (button == SDL_CONTROLLER_BUTTON_B) {
        if (app.playback_overlay != PlaybackOverlay::menu) {
            open_playback_overlay(app, PlaybackOverlay::menu);
        } else {
            app.playback_overlay = PlaybackOverlay::none;
            refresh_playback_overlay(app);
        }
        return true;
    }
    if (app.playback_overlay == PlaybackOverlay::controls) {
        return true;
    }
    if (app.playback_overlay == PlaybackOverlay::settings) {
        if (button == SDL_CONTROLLER_BUTTON_DPAD_UP ||
            button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            const int direction =
                button == SDL_CONTROLLER_BUTTON_DPAD_UP ? -1 : 1;
            app.playback_overlay_selected =
                (app.playback_overlay_selected + 12 + direction) % 12;
            refresh_playback_overlay(app);
            return true;
        }
        if (button == SDL_CONTROLLER_BUTTON_X) {
            const bfplayer::PlayerSettings previous = app.settings;
            const bfplayer::VideoScaleMode previous_scale =
                app.video_scale_mode;
            const bfplayer::VideoAspectMode previous_aspect =
                app.video_aspect_mode;
            const bfplayer::VideoCropMode previous_crop =
                app.video_crop_mode;
            bfplayer::PlayerSettings defaults{};
            defaults.subdl_api_key = app.settings.subdl_api_key;
            defaults.subtitle_languages =
                app.settings.subtitle_languages;
            app.settings = std::move(defaults);
            app.video_scale_mode = bfplayer::VideoScaleMode::fit;
            app.video_aspect_mode =
                bfplayer::VideoAspectMode::default_ratio;
            app.video_crop_mode =
                bfplayer::VideoCropMode::default_crop;
            if (!persist_active_player_settings(app)) {
                app.settings = previous;
                app.video_scale_mode = previous_scale;
                app.video_aspect_mode = previous_aspect;
                app.video_crop_mode = previous_crop;
                app.osd.show("Unable to save playback settings", 8000);
            }
            refresh_playback_overlay(app);
            return true;
        }
        if (button != SDL_CONTROLLER_BUTTON_A &&
            button != SDL_CONTROLLER_BUTTON_DPAD_LEFT &&
            button != SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
            return true;
        }
        if (button == SDL_CONTROLLER_BUTTON_A &&
            app.playback_overlay_selected == 9) {
            begin_text_edit(app, TextEditMode::subdl_api_key);
            return true;
        }
        if (button == SDL_CONTROLLER_BUTTON_A &&
            app.playback_overlay_selected == 10) {
            begin_text_edit(app, TextEditMode::subtitle_languages);
            return true;
        }
        if ((app.playback_overlay_selected == 9 ||
             app.playback_overlay_selected == 10) &&
            button != SDL_CONTROLLER_BUTTON_A) {
            return true;
        }
        const int direction =
            button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : 1;
        const bfplayer::PlayerSettings previous = app.settings;
        const bfplayer::VideoScaleMode previous_scale =
            app.video_scale_mode;
        const bfplayer::VideoAspectMode previous_aspect =
            app.video_aspect_mode;
        const bfplayer::VideoCropMode previous_crop =
            app.video_crop_mode;
        switch (app.playback_overlay_selected) {
            case 0:
                app.settings.volume_percent = std::clamp(
                    app.settings.volume_percent + direction * 5,
                    0,
                    100);
                break;
            case 1:
                app.settings.short_seek_seconds =
                    bfplayer::next_short_seek_seconds(
                        app.settings.short_seek_seconds,
                        direction);
                break;
            case 2:
                app.settings.long_seek_seconds =
                    bfplayer::next_long_seek_seconds(
                        app.settings.long_seek_seconds,
                        direction);
                break;
            case 3:
                app.settings.osd_duration_ms =
                    bfplayer::next_osd_duration_ms(
                        app.settings.osd_duration_ms,
                        direction);
                break;
            case 4:
                app.video_scale_mode =
                    bfplayer::step_video_scale_mode(
                        app.video_scale_mode,
                        direction);
                break;
            case 5:
                app.video_aspect_mode =
                    bfplayer::step_video_aspect_mode(
                        app.video_aspect_mode,
                        direction);
                break;
            case 6:
                app.video_crop_mode =
                    bfplayer::step_video_crop_mode(
                        app.video_crop_mode,
                        direction);
                break;
            case 7:
                app.settings.resume_playback =
                    !app.settings.resume_playback;
                break;
            case 8:
                app.settings.auto_subtitles =
                    !app.settings.auto_subtitles;
                break;
            case 11: {
                bfplayer::PlayerSettings defaults{};
                defaults.subdl_api_key =
                    app.settings.subdl_api_key;
                defaults.subtitle_languages =
                    app.settings.subtitle_languages;
                app.settings = std::move(defaults);
                app.video_scale_mode = bfplayer::VideoScaleMode::fit;
                app.video_aspect_mode =
                    bfplayer::VideoAspectMode::default_ratio;
                app.video_crop_mode =
                    bfplayer::VideoCropMode::default_crop;
                break;
            }
            default:
                return true;
        }
        if (!persist_active_player_settings(app)) {
            app.settings = previous;
            app.video_scale_mode = previous_scale;
            app.video_aspect_mode = previous_aspect;
            app.video_crop_mode = previous_crop;
            app.osd.show("Unable to save playback settings", 8000);
        }
        refresh_playback_overlay(app);
        return true;
    }
    if (button == SDL_CONTROLLER_BUTTON_DPAD_UP ||
        button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
        const int direction =
            button == SDL_CONTROLLER_BUTTON_DPAD_UP ? -1 : 1;
        app.playback_overlay_selected =
            (app.playback_overlay_selected + 7 + direction) % 7;
        refresh_playback_overlay(app);
        return true;
    }
    if (app.playback_overlay_selected == 5 &&
        (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ||
         button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
        adjust_subtitle_delay(
            app,
            button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -100 : 100);
        refresh_playback_overlay(app);
        return true;
    }
    if (button != SDL_CONTROLLER_BUTTON_A) {
        return true;
    }
    switch (app.playback_overlay_selected) {
        case 0:
            app.playback_overlay = PlaybackOverlay::none;
            refresh_playback_overlay(app);
            break;
        case 1:
            open_playback_overlay(app, PlaybackOverlay::subtitles);
            break;
        case 2:
            open_playback_overlay(app, PlaybackOverlay::controls);
            break;
        case 3:
            open_playback_overlay(app, PlaybackOverlay::settings);
            break;
        case 4:
            toggle_mute(app);
            refresh_playback_overlay(app);
            break;
        case 5:
            app.subtitle_delay_ms = 0;
            app.osd.show("Subtitle timing reset to 0 ms");
            refresh_playback_overlay(app);
            break;
        case 6:
            app.playback_running = false;
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::info,
                "playback-stop requested=menu");
            break;
        default:
            break;
    }
    return true;
}

void on_controller_button(App& app, SDL_GameControllerButton button) {
    if (handle_playback_overlay_button(app, button)) {
        return;
    }
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A:
            toggle_pause(app);
            break;
        case SDL_CONTROLLER_BUTTON_B:
            cycle_subtitles(app);
            break;
        case SDL_CONTROLLER_BUTTON_X:
            cycle_stream(app, KIT_STREAMTYPE_AUDIO, false);
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            if (app.controller) {
                const bool left_trigger =
                    SDL_GameControllerGetAxis(
                        app.controller,
                        SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
                const bool right_trigger =
                    SDL_GameControllerGetAxis(
                        app.controller,
                        SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
                if (left_trigger && right_trigger) {
                    cycle_video_scale(app);
                } else if (right_trigger) {
                    cycle_video_aspect(app);
                } else if (left_trigger) {
                    cycle_video_crop(app);
                } else {
                    cycle_stream(app, KIT_STREAMTYPE_VIDEO, false);
                }
            } else {
                cycle_stream(app, KIT_STREAMTYPE_VIDEO, false);
            }
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            seek_relative(
                app,
                -static_cast<double>(app.settings.short_seek_seconds));
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            seek_relative(
                app,
                static_cast<double>(app.settings.short_seek_seconds));
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            seek_relative(
                app,
                -static_cast<double>(app.settings.long_seek_seconds));
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            seek_relative(
                app,
                static_cast<double>(app.settings.long_seek_seconds));
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            seek_chapter(app, -1);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            seek_chapter(app, 1);
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            set_volume(app, app.volume_percent - 5);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            set_volume(app, app.volume_percent + 5);
            break;
        case bfplayer::kControllerTouchpadButton:
            open_playback_overlay(app, PlaybackOverlay::controls);
            break;
#if !defined(BFPLAYER_PS5)
        case SDL_CONTROLLER_BUTTON_BACK:
            toggle_mute(app);
            break;
#endif
        case bfplayer::kControllerOptionsButton:
            open_playback_overlay(app, PlaybackOverlay::menu);
            break;
        default:
            break;
    }
}

void pump_events(App& app) {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        app.redraw_requested = true;
        if (event.type == SDL_QUIT) {
            app.running = false;
            app.playback_running = false;
            continue;
        }
        if (app.text_edit_mode != TextEditMode::none) {
            if (event.type == SDL_TEXTINPUT) {
                const std::size_t maximum =
                    app.text_edit_mode == TextEditMode::subdl_api_key
                    ? 256
                    : (app.text_edit_mode ==
                               TextEditMode::subtitle_search_query
                           ? 256
                           : 96);
                app.text_edit_buffer.append(event.text.text);
                while (app.text_edit_buffer.size() > maximum) {
                    erase_last_utf8_codepoint(app.text_edit_buffer);
                }
                refresh_playback_overlay(app);
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_RETURN ||
                    event.key.keysym.sym == SDLK_KP_ENTER) {
                    finish_text_edit(app, true);
                } else if (event.key.keysym.sym == SDLK_ESCAPE) {
                    finish_text_edit(app, false);
                } else if (event.key.keysym.sym == SDLK_BACKSPACE) {
                    erase_last_utf8_codepoint(app.text_edit_buffer);
                    refresh_playback_overlay(app);
                } else if (event.key.keysym.sym == SDLK_DELETE) {
                    app.text_edit_buffer.clear();
                    refresh_playback_overlay(app);
                }
            }
            // Do not let IME controller events also change playback.
            continue;
        }
        switch (event.type) {
            case SDL_CONTROLLERDEVICEADDED:
                if (!app.controller && SDL_IsGameController(event.cdevice.which)) {
                    app.controller = SDL_GameControllerOpen(event.cdevice.which);
                }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (app.controller &&
                    SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(app.controller)) ==
                        event.cdevice.which) {
                    SDL_GameControllerClose(app.controller);
                    app.controller = nullptr;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (app.playback_overlay != PlaybackOverlay::none &&
                    app.text_edit_mode == TextEditMode::none) {
                    if (event.cbutton.button ==
                        SDL_CONTROLLER_BUTTON_DPAD_UP) {
                        app.navigation_repeat.press(
                            -1,
                            SDL_GetTicks64());
                    } else if (
                        event.cbutton.button ==
                        SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                        app.navigation_repeat.press(
                            1,
                            SDL_GetTicks64());
                    }
                }
                on_controller_button(
                    app, static_cast<SDL_GameControllerButton>(event.cbutton.button));
                break;
            case SDL_CONTROLLERBUTTONUP:
                if (event.cbutton.button ==
                    SDL_CONTROLLER_BUTTON_DPAD_UP) {
                    app.navigation_repeat.release(-1);
                } else if (
                    event.cbutton.button ==
                    SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                    app.navigation_repeat.release(1);
                }
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    if (app.playback_overlay == PlaybackOverlay::none) {
                        open_playback_overlay(app, PlaybackOverlay::menu);
                    } else {
                        app.playback_overlay = PlaybackOverlay::none;
                        refresh_playback_overlay(app);
                    }
                } else if (event.key.keysym.sym == SDLK_F1) {
                    open_playback_overlay(app, PlaybackOverlay::controls);
                } else if (app.playback_overlay != PlaybackOverlay::none) {
                    if (event.key.keysym.sym == SDLK_UP) {
                        on_controller_button(app, SDL_CONTROLLER_BUTTON_DPAD_UP);
                    } else if (event.key.keysym.sym == SDLK_DOWN) {
                        on_controller_button(app, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
                    } else if (event.key.keysym.sym == SDLK_LEFT) {
                        on_controller_button(app, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
                    } else if (event.key.keysym.sym == SDLK_RIGHT) {
                        on_controller_button(app, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
                    } else if (event.key.keysym.sym == SDLK_RETURN) {
                        on_controller_button(app, SDL_CONTROLLER_BUTTON_A);
                    } else if (event.key.keysym.sym == SDLK_BACKSPACE) {
                        on_controller_button(app, SDL_CONTROLLER_BUTTON_B);
                    } else if (event.key.keysym.sym == SDLK_x) {
                        on_controller_button(app, SDL_CONTROLLER_BUTTON_X);
                    }
                } else if (event.key.keysym.sym == SDLK_SPACE) {
                    toggle_pause(app);
                } else if (event.key.keysym.sym == SDLK_LEFT) {
                    seek_relative(
                        app,
                        -static_cast<double>(app.settings.short_seek_seconds));
                } else if (event.key.keysym.sym == SDLK_RIGHT) {
                    seek_relative(
                        app,
                        static_cast<double>(app.settings.short_seek_seconds));
                } else if (event.key.keysym.sym == SDLK_z) {
                    cycle_video_scale(app);
                } else if (event.key.keysym.sym == SDLK_a) {
                    cycle_video_aspect(app);
                } else if (event.key.keysym.sym == SDLK_c) {
                    cycle_video_crop(app);
                }
                break;
            default:
                break;
        }
    }
    if (app.playback_overlay == PlaybackOverlay::none ||
        app.text_edit_mode != TextEditMode::none) {
        app.navigation_repeat.reset();
        return;
    }
    const int repeated =
        app.navigation_repeat.poll(SDL_GetTicks64());
    const SDL_GameControllerButton repeated_button =
        repeated < 0
        ? SDL_CONTROLLER_BUTTON_DPAD_UP
        : SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    for (int index = 0; index < std::abs(repeated); ++index) {
        on_controller_button(app, repeated_button);
    }
}

void update_text_input_state(App& app) {
#if defined(BFPLAYER_PS5)
    if (app.text_edit_mode == TextEditMode::none) {
        return;
    }
    SDL_Window* window = SDL_RenderGetWindow(app.renderer);
    const bool shown =
        window &&
        SDL_IsScreenKeyboardShown(window) == SDL_TRUE;
    if (shown) {
        app.ime_was_visible = true;
    } else if (app.ime_was_visible) {
        // The PS5 backend emits text + Return for OK, but no event for cancel.
        finish_text_edit(app, false);
    }
#else
    (void)app;
#endif
}

PlaybackOutcome run_player(
    App& app,
    const char* path,
    const char* explicit_subtitle = nullptr) {
    // Geometry overrides are playback-local. A saved stretch/fill/crop mode
    // must never make a newly opened video's "Original" ratio look distorted.
    app.video_scale_mode = bfplayer::VideoScaleMode::fit;
    app.video_aspect_mode = bfplayer::VideoAspectMode::default_ratio;
    app.video_crop_mode = bfplayer::VideoCropMode::default_crop;
    app.source_display_aspect = 0.0;
    app.current_media_path = path ? bfplayer::redact_uri_secrets(path) : std::string{};
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-start path=%s explicit_subtitle=%s",
        app.current_media_path.c_str(),
        explicit_subtitle && explicit_subtitle[0]
            ? bfplayer::redact_uri_secrets(explicit_subtitle).c_str()
            : "<none>");
    app.playback_running = true;
    app.paused = false;
    app.subtitle_delay_ms = 0;
    app.playback_overlay = PlaybackOverlay::none;
    app.playback_overlay_selected = 0;
    const bool opening_osd_ready =
        app.osd.open(app.renderer, app.fallback_font);
    if (opening_osd_ready) {
        SDL_SetRenderDrawColor(app.renderer, 8, 13, 25, 255);
        SDL_RenderClear(app.renderer);
        app.osd.show("Opening media...", 60000);
        app.osd.render(0.0, 0.0, false);
        SDL_RenderPresent(app.renderer);
    }
    const auto show_open_error = [&](const std::string& message) {
        if (!opening_osd_ready) {
            return;
        }
        const Uint64 end = SDL_GetTicks64() + 2200;
        app.osd.show("Unable to play: " + message, 2200);
        while (SDL_GetTicks64() < end) {
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_CONTROLLERBUTTONDOWN &&
                     event.cbutton.button ==
                         bfplayer::kControllerOptionsButton)) {
                    return;
                }
            }
            SDL_SetRenderDrawColor(app.renderer, 8, 13, 25, 255);
            SDL_RenderClear(app.renderer);
            app.osd.render(0.0, 0.0, false);
            SDL_RenderPresent(app.renderer);
            SDL_Delay(16);
        }
    };
    std::string source_error;
    app.source = create_bounded_source(app, path, source_error);
    if (!app.source) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "playback-start failed path=%s error=%s",
            app.current_media_path.c_str(),
            source_error.c_str());
        show_open_error(source_error);
        return PlaybackOutcome::error;
    }
    bfplayer::LibraryDatabase resume_database;
    const bool database_available =
        resume_database.open("/data/BFplayer/library.db");
    app.settings = database_available
        ? load_player_settings(resume_database)
        : bfplayer::PlayerSettings{};
    app.volume_percent = app.settings.volume_percent;
    app.previous_volume_percent =
        app.volume_percent > 0 ? app.volume_percent : 100;
    app.osd.set_default_duration(
        static_cast<std::uint64_t>(app.settings.osd_duration_ms));
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "player-settings volume=%d short_seek=%d long_seek=%d osd_ms=%d resume=%d auto_subtitles=%d",
        app.settings.volume_percent,
        app.settings.short_seek_seconds,
        app.settings.long_seek_seconds,
        app.settings.osd_duration_ms,
        app.settings.resume_playback ? 1 : 0,
        app.settings.auto_subtitles ? 1 : 0);
    app.subtitle_sidecars = bfplayer::is_network_uri(path)
        ? std::vector<std::string>{}
        : bfplayer::find_subtitle_sidecars(path);
    if (explicit_subtitle && explicit_subtitle[0]) {
        app.subtitle_sidecars.insert(app.subtitle_sidecars.begin(), explicit_subtitle);
    }
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "subtitle-sidecars count=%zu",
        app.subtitle_sidecars.size());
    if (const auto* format = static_cast<const AVFormatContext*>(app.source->format_ctx)) {
        for (unsigned int index = 0; index < format->nb_streams; ++index) {
            const AVMediaType type = format->streams[index]->codecpar->codec_type;
            if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO ||
                type == AVMEDIA_TYPE_SUBTITLE) {
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::info,
                    "track index=%u type=%d description=%s",
                    index,
                    type,
                    track_description(app, static_cast<int>(index)).c_str());
            }
        }
    }

    Kit_AudioFormatRequest audio_request{};
    Kit_ResetAudioFormatRequest(&audio_request);
    // SDL_kitchensink 2.0 only maps U8/S16/S32 SDL formats to FFmpeg.
    // Requesting F32 produces AV_SAMPLE_FMT_NONE and makes swr_init fail.
    audio_request.format = AUDIO_S16SYS;
    audio_request.is_signed = 1;
    audio_request.bytes = 2;
    audio_request.sample_rate = 48000;
    audio_request.channels = 2;

    const int initial_video =
        Kit_GetBestSourceStream(app.source, KIT_STREAMTYPE_VIDEO);
    const int initial_audio =
        Kit_GetBestSourceStream(app.source, KIT_STREAMTYPE_AUDIO);
    const bool network_source = bfplayer::is_network_uri(path);
    const PlaybackBufferPolicy buffer_policy =
        playback_buffer_policy(network_source);
    apply_playback_buffer_policy(buffer_policy);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-buffer-policy kind=%s decoder_threads=%d video_packets=%d video_frames=%d audio_packets=%d audio_frames=%d peak_rss_kib=%llu",
        network_source ? "network" : "local",
        Kit_GetHint(KIT_HINT_THREAD_COUNT),
        buffer_policy.video_packets,
        buffer_policy.video_frames,
        buffer_policy.audio_packets,
        buffer_policy.audio_frames,
        static_cast<unsigned long long>(process_peak_rss_kib()));
    const VideoSourceInfo source_video = inspect_video_source(
        static_cast<const AVFormatContext*>(app.source->format_ctx),
        initial_video);
    app.source_frame_rate = source_video.frame_rate;
    app.source_width = source_video.width;
    app.source_height = source_video.height;
    app.source_hdr = source_video.hdr_source;
    app.source_hdr_transfer = source_video.color_transfer;
    app.source_display_aspect = source_video.display_aspect;
    SDL_YUV_CONVERSION_MODE yuv_conversion_mode =
        SDL_YUV_CONVERSION_AUTOMATIC;
    if (source_video.hdr_source) {
        // The decoder tone-maps HDR to limited-range BT.709 SDR in place.
        yuv_conversion_mode = SDL_YUV_CONVERSION_BT709;
    } else if (source_video.color_range_value == AVCOL_RANGE_JPEG) {
        yuv_conversion_mode = SDL_YUV_CONVERSION_JPEG;
    } else if (source_video.color_space_value == AVCOL_SPC_BT709) {
        yuv_conversion_mode = SDL_YUV_CONVERSION_BT709;
    }
    SDL_SetYUVConversionMode(yuv_conversion_mode);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "video-color-output source_hdr=%d sdl_yuv_mode=%d hdr_policy=%s",
        source_video.hdr_source ? 1 : 0,
        static_cast<int>(yuv_conversion_mode),
        source_video.hdr_source
            ? "software-tone-map-bt709-limited"
            : "passthrough");
    const bool wants_true_4k =
        source_video.width > kWindowWidth ||
        source_video.height > kWindowHeight;
    if (wants_true_4k) {
        (void)switch_fullscreen_output(
            app,
            kPreferredOutputWidth,
            kPreferredOutputHeight,
            "source-above-1080p");
    } else {
        (void)switch_fullscreen_output(
            app,
            kWindowWidth,
            kWindowHeight,
            "source-at-or-below-1080p");
    }
    const Kit_VideoFormatRequest video_request =
        make_video_format_request(
            source_video,
            app.output_width,
            app.output_height);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "video-source codec=%s profile=%d pix_fmt=%s bit_depth=%d size=%dx%d sar=%d:%d dar=%.6f fps=%.3f bitrate=%lld range=%s colorspace=%s transfer=%s primaries=%s hdr=%d demanding_software_decode=%d",
        source_video.codec,
        source_video.profile,
        source_video.pixel_format,
        source_video.bit_depth,
        source_video.width,
        source_video.height,
        source_video.sample_aspect_numerator,
        source_video.sample_aspect_denominator,
        source_video.display_aspect,
        source_video.frame_rate,
        static_cast<long long>(source_video.bit_rate),
        source_video.color_range,
        source_video.color_space,
        source_video.color_transfer,
        source_video.color_primaries,
        source_video.hdr_source ? 1 : 0,
        source_video.demanding_software_decode ? 1 : 0);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "video-output-request source=%dx%d output=%dx%d display=%dx%d downscale=%d true_4k=%d",
        source_video.width,
        source_video.height,
        video_request.width,
        video_request.height,
        app.output_width,
        app.output_height,
        video_request.width > 0 && video_request.height > 0 ? 1 : 0,
        app.true_4k_output ? 1 : 0);
    // Kitchensink requires a video decoder for subtitle layout. Audio-only
    // files with timed lyrics must still open instead of failing creation.
    const int initial_subtitle =
        initial_video >= 0 && app.settings.auto_subtitles
        ? Kit_GetBestSourceStream(app.source, KIT_STREAMTYPE_SUBTITLE)
        : -1;
    app.player = Kit_CreatePlayer(
        app.source,
        initial_video,
        initial_audio,
        initial_subtitle,
        &video_request,
        &audio_request,
        kWindowWidth,
        kWindowHeight);
    if (!app.player) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "player-create failed error=%s",
            Kit_GetError());
        show_open_error(Kit_GetError());
        return PlaybackOutcome::error;
    }
    if (Kit_GetPlayerVideoStream(app.player) < 0 &&
        Kit_GetPlayerAudioStream(app.player) < 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "player-create failed reason=no-decodable-audio-video");
        return PlaybackOutcome::error;
    }

    if (!open_audio(app) || !create_video_textures(app)) {
        show_open_error(SDL_GetError());
        return PlaybackOutcome::error;
    }
    if (!opening_osd_ready) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::warning,
            "osd-open failed error=%s",
            app.osd.error().c_str());
    }

    const bool source_persistence_allowed =
        !bfplayer::uri_has_sensitive_components(path);
    bfplayer::ResumeState saved_resume{};
    double pending_resume_seconds = 0.0;
    if (app.settings.resume_playback &&
        database_available &&
        source_persistence_allowed &&
        resume_database.load_resume(path, saved_resume)) {
        const double resume_seconds = static_cast<double>(saved_resume.position_ms) / 1000.0;
        const double duration = Kit_GetPlayerDuration(app.player);
        if (std::isfinite(duration) && !saved_resume.completed &&
            resume_seconds >= 10.0 &&
            resume_seconds < duration - 30.0) {
            // Kitchensink ignores/rejects seeks while the player is stopped.
            // Apply this immediately after Kit_PlayerPlay() below.
            pending_resume_seconds = resume_seconds;
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::info,
                "resume-pending seconds=%.3f duration=%.3f",
                resume_seconds,
                duration);
        } else {
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::info,
                "resume-skipped saved_ms=%lld duration=%.3f completed=%d",
                static_cast<long long>(saved_resume.position_ms),
                duration,
                saved_resume.completed ? 1 : 0);
        }
    } else if (app.settings.resume_playback && database_available &&
               source_persistence_allowed) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "resume-not-found");
    }
    bfplayer::TrackPreferences saved_preferences{};
    if (database_available && source_persistence_allowed &&
        resume_database.load_track_preferences(path, saved_preferences)) {
        restore_track_preferences(
            app,
            saved_preferences,
            explicit_subtitle && explicit_subtitle[0]);
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "preferences-restored audio=%d subtitle=%d external=%s delay_ms=%lld",
            saved_preferences.audio_stream,
            saved_preferences.subtitle_stream,
            bfplayer::redact_uri_secrets(saved_preferences.external_subtitle).c_str(),
            static_cast<long long>(app.subtitle_delay_ms));
    }
    if (explicit_subtitle && explicit_subtitle[0]) {
        open_external_subtitle(app, 0);
    }

    Kit_PlayerInfo info{};
    Kit_GetPlayerInfo(app.player, &info);
    int sample_aspect_numerator = 0;
    int sample_aspect_denominator = 0;
    const int sample_aspect_status = Kit_GetPlayerAspectRatio(
        app.player,
        &sample_aspect_numerator,
        &sample_aspect_denominator);
    const double initial_display_aspect =
        bfplayer::display_aspect_from_sample_aspect(
            info.video_format.width,
            info.video_format.height,
            sample_aspect_numerator,
            sample_aspect_denominator);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-format video=%s threads=%d frame_threading=%u slice_threading=%u output=%dx%d format=%u audio=%s threads=%d %dHz/%dch duration_s=%.3f",
        info.video_codec.name,
        info.video_codec.threads,
        info.video_codec.frame_threading,
        info.video_codec.slice_threading,
        info.video_format.width,
        info.video_format.height,
        static_cast<unsigned int>(info.video_format.format),
        info.audio_codec.name,
        info.audio_codec.threads,
        info.audio_format.sample_rate,
        info.audio_format.channels,
        Kit_GetPlayerDuration(app.player));
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-aspect frame=%dx%d sar=%d:%d sar_status=%d dar=%.6f",
        info.video_format.width,
        info.video_format.height,
        sample_aspect_numerator,
        sample_aspect_denominator,
        sample_aspect_status,
        initial_display_aspect);

    Kit_PlayerPlay(app.player);
    if (pending_resume_seconds > 0.0) {
        if (Kit_PlayerSeek(app.player, pending_resume_seconds) == 0) {
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::info,
                "resume-applied seconds=%.3f actual=%.3f",
                pending_resume_seconds,
                Kit_GetPlayerPosition(app.player));
            app.osd.show(
                "Resumed at " +
                    std::to_string(
                        static_cast<long long>(pending_resume_seconds)) +
                    " seconds",
                5000);
        } else {
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::error,
                "resume-seek failed seconds=%.3f error=%s",
                pending_resume_seconds,
                Kit_GetError());
        }
    } else if (source_video.demanding_software_decode) {
        app.osd.show(
            "High-load 4K video: software decoding may drop frames",
            10000);
    } else {
        app.osd.show(
            "Cross Play/Pause   D-pad Seek   Touchpad Controls   Options Menu",
            6000);
    }
    Uint64 last_resume_save = SDL_GetTicks64();
    Uint64 last_diagnostics = last_resume_save;
    Uint64 last_remote_status = last_resume_save;
    Uint64 last_visual_render = 0;
    const Uint64 performance_frequency = SDL_GetPerformanceFrequency();
    TimingWindow loop_timing;
    TimingWindow audio_pull_timing;
    TimingWindow video_pull_timing;
    TimingWindow render_timing;
    TimingWindow present_timing;
    double active_playback_ms = 0.0;
    struct rusage diagnostics_usage {};
    (void)getrusage(RUSAGE_SELF, &diagnostics_usage);
    bfplayer::RemotePlaybackStatus remote_status;
    remote_status.phase = "playback";
    remote_status.media_path = app.current_media_path;
    remote_status.playing = true;
    remote_status.source_fps = source_video.frame_rate;
    remote_status.source_width = source_video.width;
    remote_status.source_height = source_video.height;
    remote_status.output_width = info.video_format.width;
    remote_status.output_height = info.video_format.height;
    remote_status.hdr_source = source_video.hdr_source;
    remote_status.hdr_transfer = source_video.color_transfer;
    std::uint64_t video_updates = 0;
    std::uint64_t video_empty_polls = 0;
    static_assert(kAudioBufferBytes % sizeof(std::int16_t) == 0);
    std::array<std::int16_t, kAudioBufferBytes / sizeof(std::int16_t)> audio_buffer{};
    std::array<SDL_Rect, kSubtitleFragments> subtitle_sources{};
    std::array<SDL_Rect, kSubtitleFragments> subtitle_targets{};

    while (app.running && app.playback_running &&
           Kit_GetPlayerState(app.player) != KIT_STOPPED) {
        const Uint64 loop_started = SDL_GetPerformanceCounter();
        pump_events(app);
        consume_remote_commands(app, true);
        update_text_input_state(app);
        consume_subtitle_job(app);

        if (!app.paused && app.audio != 0) {
            const Uint32 queued = SDL_GetQueuedAudioSize(app.audio);
            if (queued < kAudioBufferBytes) {
                const std::size_t available =
                    static_cast<std::size_t>(kAudioBufferBytes - queued);
                const Uint64 audio_pull_started = SDL_GetPerformanceCounter();
                const int got = Kit_GetPlayerAudioData(
                    app.player,
                    queued,
                    reinterpret_cast<unsigned char*>(audio_buffer.data()),
                    available);
                audio_pull_timing.add(elapsed_performance_ms(
                    audio_pull_started,
                    SDL_GetPerformanceCounter(),
                    performance_frequency));
                if (got > 0) {
                    if (static_cast<std::size_t>(got) > available ||
                        got % static_cast<int>(sizeof(std::int16_t)) != 0) {
                        bfplayer::diagnostics_log(
                            bfplayer::DiagnosticLevel::error,
                            "audio-pull invalid-bytes got=%d available=%zu",
                            got,
                            available);
                        app.playback_running = false;
                        continue;
                    }
                    if (app.volume_percent < 100) {
                        const std::size_t sample_count =
                            static_cast<std::size_t>(got) / sizeof(std::int16_t);
                        for (std::size_t index = 0; index < sample_count; ++index) {
                            const std::int32_t scaled =
                                static_cast<std::int32_t>(audio_buffer[index]) *
                                app.volume_percent / 100;
                            audio_buffer[index] = static_cast<std::int16_t>(scaled);
                        }
                    }
                    if (SDL_QueueAudio(
                            app.audio,
                            audio_buffer.data(),
                            static_cast<Uint32>(got)) != 0) {
                        log_sdl("SDL_QueueAudio");
                        app.playback_running = false;
                    }
                }
            }
        }

        bool video_frame_updated = false;
        if (app.video && !app.paused) {
            const Uint64 video_pull_started = SDL_GetPerformanceCounter();
            const int video_result = Kit_GetPlayerVideoSDLTexture(
                app.player, app.video, nullptr);
            video_pull_timing.add(elapsed_performance_ms(
                video_pull_started,
                SDL_GetPerformanceCounter(),
                performance_frequency));
            if (video_result == 0) {
                video_frame_updated = true;
                ++video_updates;
            } else {
                ++video_empty_polls;
            }
        }
        Uint64 now = SDL_GetTicks64();
        const bool audio_only_refresh =
            !app.video && now - last_visual_render >= 33;
        const bool should_render =
            app.redraw_requested || video_frame_updated ||
            audio_only_refresh;
        if (should_render) {
            const Uint64 render_started = SDL_GetPerformanceCounter();
            SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
            SDL_RenderClear(app.renderer);
            if (app.video) {
                const bfplayer::VideoLayout layout = player_video_layout(app);
                const SDL_Rect source{
                    layout.source.x,
                    layout.source.y,
                    layout.source.width,
                    layout.source.height};
                const SDL_Rect destination{
                    layout.destination.x,
                    layout.destination.y,
                    layout.destination.width,
                    layout.destination.height};
                SDL_RenderCopy(
                    app.renderer,
                    app.video,
                    layout.crop_source ? &source : nullptr,
                    &destination);
            }
            if (app.subtitles) {
                const double subtitle_clock =
                    Kit_GetPlayerPosition(app.player) -
                    static_cast<double>(app.subtitle_delay_ms) / 1000.0;
                const int fragments = bfplayer_get_player_subtitle_texture_at(
                    app.player,
                    app.subtitles,
                    subtitle_sources.data(),
                    subtitle_targets.data(),
                    static_cast<int>(subtitle_sources.size()),
                    subtitle_clock);
                const int safe_fragments = std::clamp(
                    fragments,
                    0,
                    static_cast<int>(subtitle_sources.size()));
                for (int i = 0; i < safe_fragments; ++i) {
                    SDL_RenderCopy(
                        app.renderer,
                        app.subtitles,
                        &subtitle_sources[static_cast<std::size_t>(i)],
                        &subtitle_targets[static_cast<std::size_t>(i)]);
                }
            }
            if (app.external_subtitles.is_open()) {
                const auto position_ms = seconds_to_milliseconds(
                    Kit_GetPlayerPosition(app.player));
                if (!app.external_subtitles.render(
                        position_ms, app.subtitle_delay_ms)) {
                    std::fprintf(
                        stderr, "External subtitle render: %s\n",
                        app.external_subtitles.error().c_str());
                    bfplayer::diagnostics_log(
                        bfplayer::DiagnosticLevel::error,
                        "subtitle-render failed error=%s",
                        app.external_subtitles.error().c_str());
                    app.external_subtitles.close();
                    app.external_subtitle_index = -1;
                }
            }
            app.osd.render(
                Kit_GetPlayerPosition(app.player),
                Kit_GetPlayerDuration(app.player),
                app.paused);
            render_timing.add(elapsed_performance_ms(
                render_started,
                SDL_GetPerformanceCounter(),
                performance_frequency));
            const Uint64 present_started = SDL_GetPerformanceCounter();
            SDL_RenderPresent(app.renderer);
            present_timing.add(elapsed_performance_ms(
                present_started,
                SDL_GetPerformanceCounter(),
                performance_frequency));
            app.redraw_requested = false;
            last_visual_render = now;
        } else {
            SDL_Delay(app.paused ? 8 : 1);
        }
        now = SDL_GetTicks64();
        const double current_loop_ms = elapsed_performance_ms(
            loop_started,
            SDL_GetPerformanceCounter(),
            performance_frequency);
        loop_timing.add(current_loop_ms);
        if (!app.paused) {
            active_playback_ms += current_loop_ms;
        }
        if (now - last_diagnostics >= 5000) {
            const double diagnostics_seconds =
                static_cast<double>(now - last_diagnostics) / 1000.0;
            const double video_update_rate = diagnostics_seconds > 0.0
                ? static_cast<double>(video_updates) / diagnostics_seconds
                : 0.0;
            const double expected_frames =
                source_video.frame_rate > 0.0
                ? source_video.frame_rate * active_playback_ms / 1000.0
                : 0.0;
            const std::uint64_t missed_frames =
                expected_frames > static_cast<double>(video_updates)
                ? static_cast<std::uint64_t>(std::ceil(
                      expected_frames - static_cast<double>(video_updates)))
                : 0;
            const Uint32 audio_queued =
                app.audio != 0 ? SDL_GetQueuedAudioSize(app.audio) : 0U;
            const std::uint64_t audio_queue_ms =
                static_cast<std::uint64_t>(audio_queued) * 1000ULL /
                kAudioBytesPerSecond;
            struct rusage current_usage {};
            (void)getrusage(RUSAGE_SELF, &current_usage);
            const double cpu_seconds =
                process_cpu_seconds(current_usage) -
                process_cpu_seconds(diagnostics_usage);
            const double user_cpu_ms =
                (timeval_seconds(current_usage.ru_utime) -
                 timeval_seconds(diagnostics_usage.ru_utime)) * 1000.0;
            const double system_cpu_ms =
                (timeval_seconds(current_usage.ru_stime) -
                 timeval_seconds(diagnostics_usage.ru_stime)) * 1000.0;
            const double cpu_core_equivalents =
                diagnostics_seconds > 0.0
                ? std::max(0.0, cpu_seconds) / diagnostics_seconds
                : 0.0;
            const std::uint64_t voluntary_switches =
                current_usage.ru_nvcsw >= diagnostics_usage.ru_nvcsw
                ? static_cast<std::uint64_t>(
                      current_usage.ru_nvcsw - diagnostics_usage.ru_nvcsw)
                : 0;
            const std::uint64_t involuntary_switches =
                current_usage.ru_nivcsw >= diagnostics_usage.ru_nivcsw
                ? static_cast<std::uint64_t>(
                      current_usage.ru_nivcsw - diagnostics_usage.ru_nivcsw)
                : 0;
            const bfplayer::SafeReadFileStats file_stats =
                app.local_media.stats();
            remote_status.delivered_fps = video_update_rate;
            remote_status.loop_average_ms = loop_timing.average();
            remote_status.loop_max_ms = loop_timing.maximum_ms;
            remote_status.video_pull_average_ms =
                video_pull_timing.average();
            remote_status.video_pull_max_ms =
                video_pull_timing.maximum_ms;
            remote_status.render_average_ms = render_timing.average();
            remote_status.render_max_ms = render_timing.maximum_ms;
            remote_status.present_average_ms = present_timing.average();
            remote_status.present_max_ms = present_timing.maximum_ms;
            remote_status.present_p95_ms = present_timing.percentile(0.95);
            remote_status.present_p99_ms = present_timing.percentile(0.99);
            remote_status.audio_pull_average_ms =
                audio_pull_timing.average();
            remote_status.audio_pull_max_ms =
                audio_pull_timing.maximum_ms;
            remote_status.cpu_core_equivalents = cpu_core_equivalents;
            remote_status.user_cpu_ms = std::max(0.0, user_cpu_ms);
            remote_status.system_cpu_ms = std::max(0.0, system_cpu_ms);
            remote_status.voluntary_context_switches = voluntary_switches;
            remote_status.involuntary_context_switches =
                involuntary_switches;
            remote_status.video_updates = video_updates;
            remote_status.video_empty_polls = video_empty_polls;
            remote_status.estimated_missed_frames = missed_frames;
            remote_status.peak_rss_kib = process_peak_rss_kib();
            remote_status.media_bytes_read = file_stats.bytes_read;
            remote_status.media_read_calls = file_stats.read_calls;
            remote_status.media_read_time_us = file_stats.read_time_us;
            remote_status.media_seek_calls = file_stats.seek_calls;
            remote_status.media_seek_time_us = file_stats.seek_time_us;
            remote_status.audio_queued_bytes = audio_queued;
            update_decoder_buffer_status(app, remote_status);
            const Kit_VideoToneMapInfo tone_map_info =
                update_tone_map_status(app, remote_status);
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::info,
                "playback-heartbeat path=%s position_s=%.3f duration_s=%.3f paused=%d state=%d active_ms=%.3f source_fps=%.3f video_updates=%llu video_update_fps=%.3f expected_frames=%.3f estimated_missed=%llu video_empty_polls=%llu video_frames=%u/%u video_packets=%u/%u audio_frames=%u/%u audio_packets=%u/%u audio_queued=%u audio_queue_ms=%llu peak_rss_kib=%llu cpu_cores=%.3f user_cpu_ms=%.3f system_cpu_ms=%.3f voluntary_ctx=%llu involuntary_ctx=%llu loop_avg_ms=%.3f loop_max_ms=%.3f audio_pull_avg_ms=%.3f audio_pull_max_ms=%.3f video_pull_avg_ms=%.3f video_pull_max_ms=%.3f render_avg_ms=%.3f render_max_ms=%.3f present_avg_ms=%.3f present_max_ms=%.3f present_p95_ms=%.3f present_p99_ms=%.3f present_count=%llu media_bytes=%llu media_reads=%llu media_read_ms=%.3f media_seeks=%llu media_seek_ms=%.3f external_subtitle=%d delay_ms=%lld scale=%s aspect=%s crop=%s",
                app.current_media_path.c_str(),
                Kit_GetPlayerPosition(app.player),
                Kit_GetPlayerDuration(app.player),
                app.paused ? 1 : 0,
                Kit_GetPlayerState(app.player),
                active_playback_ms,
                source_video.frame_rate,
                static_cast<unsigned long long>(video_updates),
                video_update_rate,
                expected_frames,
                static_cast<unsigned long long>(missed_frames),
                static_cast<unsigned long long>(video_empty_polls),
                remote_status.video_frames_length,
                remote_status.video_frames_capacity,
                remote_status.video_packets_length,
                remote_status.video_packets_capacity,
                remote_status.audio_frames_length,
                remote_status.audio_frames_capacity,
                remote_status.audio_packets_length,
                remote_status.audio_packets_capacity,
                audio_queued,
                static_cast<unsigned long long>(audio_queue_ms),
                static_cast<unsigned long long>(remote_status.peak_rss_kib),
                cpu_core_equivalents,
                std::max(0.0, user_cpu_ms),
                std::max(0.0, system_cpu_ms),
                static_cast<unsigned long long>(voluntary_switches),
                static_cast<unsigned long long>(involuntary_switches),
                loop_timing.average(),
                loop_timing.maximum_ms,
                audio_pull_timing.average(),
                audio_pull_timing.maximum_ms,
                video_pull_timing.average(),
                video_pull_timing.maximum_ms,
                render_timing.average(),
                render_timing.maximum_ms,
                present_timing.average(),
                present_timing.maximum_ms,
                present_timing.percentile(0.95),
                present_timing.percentile(0.99),
                static_cast<unsigned long long>(present_timing.count),
                static_cast<unsigned long long>(file_stats.bytes_read),
                static_cast<unsigned long long>(file_stats.read_calls),
                static_cast<double>(file_stats.read_time_us) / 1000.0,
                static_cast<unsigned long long>(file_stats.seek_calls),
                static_cast<double>(file_stats.seek_time_us) / 1000.0,
                app.external_subtitle_index,
                static_cast<long long>(app.subtitle_delay_ms),
                bfplayer::video_scale_mode_name(app.video_scale_mode),
                bfplayer::video_aspect_mode_name(app.video_aspect_mode),
                bfplayer::video_crop_mode_name(app.video_crop_mode));
            if (source_video.hdr_source) {
                bfplayer::diagnostics_log(
                    tone_map_info.active
                        ? bfplayer::DiagnosticLevel::info
                        : bfplayer::DiagnosticLevel::warning,
                    "hdr-tone-map active=%d transfer=%s input_range=%s input_primaries=%s source_peak_nits=%.3f target_peak_nits=%.3f output=bt709-limited workers=%u frames=%llu processing_ms=%.3f average_ms=%.6f",
                    tone_map_info.active,
                    source_video.color_transfer,
                    tone_map_info.input_full_range ? "full" : "limited",
                    tone_map_info.input_bt2020 ? "bt2020" : "bt709",
                    tone_map_info.source_peak_nits,
                    tone_map_info.target_peak_nits,
                    tone_map_info.workers,
                    tone_map_info.frames,
                    static_cast<double>(tone_map_info.processing_us) /
                        1000.0,
                    tone_map_info.frames > 0
                        ? static_cast<double>(
                              tone_map_info.processing_us) /
                              static_cast<double>(tone_map_info.frames) /
                              1000.0
                        : 0.0);
            }
            diagnostics_usage = current_usage;
            video_updates = 0;
            video_empty_polls = 0;
            active_playback_ms = 0.0;
            loop_timing.reset();
            audio_pull_timing.reset();
            video_pull_timing.reset();
            render_timing.reset();
            present_timing.reset();
            last_diagnostics = now;
        }
        if (now - last_remote_status >= 500) {
            remote_status.running = app.running;
            remote_status.playing = app.playback_running;
            remote_status.paused = app.paused;
            remote_status.player_state = Kit_GetPlayerState(app.player);
            remote_status.position_seconds =
                Kit_GetPlayerPosition(app.player);
            remote_status.duration_seconds =
                Kit_GetPlayerDuration(app.player);
            remote_status.audio_queued_bytes =
                app.audio != 0 ? SDL_GetQueuedAudioSize(app.audio) : 0U;
            remote_status.peak_rss_kib = process_peak_rss_kib();
            const bfplayer::SafeReadFileStats file_stats =
                app.local_media.stats();
            remote_status.media_bytes_read = file_stats.bytes_read;
            remote_status.media_read_calls = file_stats.read_calls;
            remote_status.media_read_time_us = file_stats.read_time_us;
            remote_status.media_seek_calls = file_stats.seek_calls;
            remote_status.media_seek_time_us = file_stats.seek_time_us;
            update_decoder_buffer_status(app, remote_status);
            update_tone_map_status(app, remote_status);
            app.remote_control.update_status(remote_status);
            last_remote_status = now;
        }
        if (database_available && source_persistence_allowed &&
            now - last_resume_save >= 10000) {
            bfplayer::ResumeState current{};
            current.position_ms = seconds_to_milliseconds(
                Kit_GetPlayerPosition(app.player));
            current.duration_ms = seconds_to_milliseconds(
                Kit_GetPlayerDuration(app.player));
            current.last_played_unix = static_cast<std::int64_t>(std::time(nullptr));
            current.completed = playback_completed(
                current.position_ms, current.duration_ms);
            if (!resume_database.save_resume(path, current)) {
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::error,
                    "resume-save failed error=%s",
                    resume_database.error().c_str());
            }
            const bfplayer::TrackPreferences preferences = current_track_preferences(app);
            if (!bfplayer::uri_has_sensitive_components(preferences.external_subtitle) &&
                !resume_database.save_track_preferences(path, preferences)) {
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::error,
                    "track-preference-save failed error=%s",
                    resume_database.error().c_str());
            }
            last_resume_save = now;
        }
    }
    if (app.text_edit_mode != TextEditMode::none) {
        finish_text_edit(app, false);
    }
    stop_subtitle_job(app);
    const double final_position = Kit_GetPlayerPosition(app.player);
    const double final_duration = Kit_GetPlayerDuration(app.player);
    const bool reached_end =
        std::isfinite(final_position) && std::isfinite(final_duration) &&
        final_duration > 0.0 &&
        final_position / final_duration >= 0.92;
    const bool finished = app.running && app.playback_running && reached_end &&
                          Kit_GetPlayerState(app.player) == KIT_STOPPED;
    if (database_available) {
        if (source_persistence_allowed) {
            bfplayer::ResumeState current{};
            // Use the values captured before any cleanup/stop transition.
            // Some decoder backends reset their public clock when stopping.
            current.position_ms = seconds_to_milliseconds(final_position);
            current.duration_ms = seconds_to_milliseconds(final_duration);
            current.last_played_unix = static_cast<std::int64_t>(std::time(nullptr));
            current.completed = playback_completed(
                current.position_ms, current.duration_ms);
            if (!resume_database.save_resume(path, current)) {
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::error,
                    "final-resume-save failed error=%s",
                    resume_database.error().c_str());
            }
            const bfplayer::TrackPreferences preferences = current_track_preferences(app);
            if (!bfplayer::uri_has_sensitive_components(
                    preferences.external_subtitle) &&
                !resume_database.save_track_preferences(path, preferences)) {
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::error,
                    "final-track-preference-save failed error=%s",
                    resume_database.error().c_str());
            }
        }
        if (!resume_database.set_settings({
                {"volume_percent", std::to_string(app.volume_percent)},
            })) {
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::error,
                "playback-setting-save failed error=%s",
                resume_database.error().c_str());
        }
    }
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-end path=%s outcome=%s position_s=%.3f duration_s=%.3f reached_end=%d",
        app.current_media_path.c_str(),
        finished ? "finished" : (app.playback_running ? "user-return" : "stopped"),
        final_position,
        final_duration,
        reached_end ? 1 : 0);
    return finished ? PlaybackOutcome::finished : PlaybackOutcome::user_return;
}

void close_media(App& app) {
    if (!app.current_media_path.empty()) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "media-close path=%s",
            app.current_media_path.c_str());
    }
    app.io_cancel.store(true, std::memory_order_relaxed);
    app.source_open_deadline_ms.store(0, std::memory_order_relaxed);
    if (app.player) {
        Kit_PlayerStop(app.player);
    }
    close_audio(app);
    app.osd.close();
    app.external_subtitles.close();
    destroy_video_textures(app);
    Kit_ClosePlayer(app.player);
    if (app.source) {
        Kit_CloseSource(app.source);
    }
    app.local_media.close();
    app.player = nullptr;
    app.source = nullptr;
    app.subtitle_sidecars.clear();
    app.subtitle_browser = {};
    app.subtitle_browser_selected = 0;
    app.subtitle_browser_first = 0;
    app.online_subtitles.clear();
    app.subtitle_online_error.clear();
    app.current_media_path.clear();
    app.external_subtitle_index = -1;
    app.playback_running = false;
    app.paused = false;
    app.playback_overlay = PlaybackOverlay::none;
    app.playback_overlay_selected = 0;
    app.text_edit_mode = TextEditMode::none;
    app.text_edit_buffer.clear();
    app.ime_was_visible = false;
    app.navigation_repeat.reset();
    SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_AUTOMATIC);
}

int run_library(
    App& app,
    const std::vector<bfplayer::MediaSource>& initial_sources = {}) {
    bfplayer::diagnostics_log(bfplayer::DiagnosticLevel::info, "library-start");
    bfplayer::LibraryUi library;
    if (!library.open(
            app.renderer,
            app.fallback_font,
            app.ui_logo,
            initial_sources)) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "library-open failed error=%s",
            library.error().c_str());
        return 1;
    }
    Uint64 last_remote_status = 0;

    while (app.running) {
        SDL_Event event{};
        bool play_selected = false;
        bool play_queue = false;
        std::string selected_path;
        consume_remote_commands(app, false);
        if (!app.pending_remote_media_path.empty()) {
            selected_path = std::move(app.pending_remote_media_path);
            app.pending_remote_media_path.clear();
            play_selected = true;
        }
        const Uint64 library_now = SDL_GetTicks64();
        if (library_now - last_remote_status >= 1000) {
            bfplayer::RemotePlaybackStatus status;
            status.phase = "library";
            status.running = app.running;
            status.output_width = app.output_width;
            status.output_height = app.output_height;
            status.peak_rss_kib = process_peak_rss_kib();
            app.remote_control.update_status(status);
            last_remote_status = library_now;
        }
        while (!play_selected && SDL_PollEvent(&event)) {
            if (event.type == SDL_CONTROLLERDEVICEADDED &&
                !app.controller &&
                SDL_IsGameController(event.cdevice.which)) {
                app.controller =
                    SDL_GameControllerOpen(event.cdevice.which);
                bfplayer::diagnostics_log(
                    app.controller
                        ? bfplayer::DiagnosticLevel::info
                        : bfplayer::DiagnosticLevel::warning,
                    "library-controller added id=%d opened=%d error=%s",
                    event.cdevice.which,
                    app.controller ? 1 : 0,
                    app.controller ? "<none>" : SDL_GetError());
            } else if (
                event.type == SDL_CONTROLLERDEVICEREMOVED &&
                app.controller &&
                SDL_JoystickInstanceID(
                    SDL_GameControllerGetJoystick(app.controller)) ==
                    event.cdevice.which) {
                SDL_GameControllerClose(app.controller);
                app.controller = nullptr;
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::info,
                    "library-controller removed id=%d",
                    event.cdevice.which);
            }
            const bfplayer::LibraryAction action = library.handle_event(event, selected_path);
            if (action == bfplayer::LibraryAction::exit) {
                app.running = false;
                break;
            }
            if (action == bfplayer::LibraryAction::play) {
                play_selected = true;
                break;
            }
            if (action == bfplayer::LibraryAction::play_queue) {
                play_selected = true;
                play_queue = true;
                break;
            }
        }
        if (!app.running) {
            break;
        }
        if (play_selected) {
            std::vector<std::string> queue;
            if (bfplayer::is_generic_playlist_path(selected_path)) {
                bfplayer::PlaylistLoadResult playlist =
                    bfplayer::load_generic_playlist(selected_path);
                if (playlist.items.empty()) {
                    const std::string error = playlist.error.empty()
                        ? "Unsupported playlist"
                        : playlist.error;
                    bfplayer::diagnostics_log(
                        bfplayer::DiagnosticLevel::error,
                        "playlist-load failed path=%s error=%s",
                        selected_path.c_str(),
                        error.c_str());
                    library.show_notice("PLAYLIST: " + error);
                    continue;
                }
                if (playlist.truncated) {
                    bfplayer::diagnostics_log(
                        bfplayer::DiagnosticLevel::warning,
                        "playlist-load truncated path=%s items=%zu",
                        selected_path.c_str(),
                        playlist.items.size());
                }
                queue = std::move(playlist.items);
            } else {
                queue = play_queue
                    ? library.playback_queue(selected_path)
                    : std::vector<std::string>{selected_path};
            }
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::info,
                "library-play-request queue_mode=%d items=%zu selected=%s",
                play_queue ? 1 : 0,
                queue.size(),
                selected_path.c_str());
            // Do not let metadata scanning compete with software video decode.
            library.close();
            for (const std::string& queued_path : queue) {
                const PlaybackOutcome outcome = run_player(app, queued_path.c_str());
                close_media(app);
                if (!app.running || outcome != PlaybackOutcome::finished) {
                    break;
                }
            }
            if (app.running) {
                (void)switch_fullscreen_output(
                    app,
                    kWindowWidth,
                    kWindowHeight,
                    "library");
            }
            if (app.running && !library.open(
                    app.renderer,
                    app.fallback_font,
                    app.ui_logo)) {
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::error,
                    "library-reopen failed error=%s",
                    library.error().c_str());
                app.running = false;
                break;
            }
            continue;
        }
        library.tick_navigation();
        library.render();
        SDL_Delay(8);
    }
    library.close();
    bfplayer::diagnostics_log(bfplayer::DiagnosticLevel::info, "library-end running=%d", app.running ? 1 : 0);
    return 0;
}

void cleanup(App& app) {
    app.remote_control.stop();
    close_media(app);
    if (app.controller) {
        SDL_GameControllerClose(app.controller);
    }
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    Kit_Quit();
    SDL_Quit();
}

void return_to_playstation_home() {
    constexpr const char* kBFplayerTitleId = "PSMC00001";
    constexpr const char* kWebsrvTitleId = "FAKE00000";
    const int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
    if (app_id <= 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "home-return failed reason=no-running-bigapp app_id=%d",
            app_id);
        return;
    }
    std::array<char, 16> title_id{};
    const int title_result =
        sceSystemServiceGetAppTitleId(app_id, title_id.data());
    if (title_result != 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "home-return failed reason=title-id app_id=%d result=0x%08x",
            app_id,
            static_cast<unsigned int>(title_result));
        return;
    }
    const bool is_bfplayer_host =
        std::strcmp(title_id.data(), kBFplayerTitleId) == 0;
    const bool is_websrv_host =
        std::strcmp(title_id.data(), kWebsrvTitleId) == 0;
    if (!is_bfplayer_host && !is_websrv_host) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "home-return skipped reason=unexpected-bigapp app_id=%d title_id=%s",
            app_id,
            title_id.data());
        return;
    }
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "home-return kill-bigapp app_id=%d title_id=%s",
        app_id,
        title_id.data());
    const int result =
        sceSystemServiceKillApp(app_id, -1, 0, 0);
    if (result != 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "home-return kill-bigapp failed app_id=%d result=0x%08x",
            app_id,
            static_cast<unsigned int>(result));
    }
}

} // namespace

int main(int argc, char** argv) {
    boot_stage_stderr("process-entry");
    PlayerInstanceLock player_instance_lock;
    boot_stage_stderr("player-lock-acquire-begin");
    const PlayerLockResult lock_result = player_instance_lock.acquire();
    if (lock_result != PlayerLockResult::acquired) {
        std::fprintf(
            stderr,
            lock_result == PlayerLockResult::already_running
                ? "BFplayer launch ignored: another player is already running\n"
                : "BFplayer launch stopped: unable to acquire player lock\n");
        return lock_result == PlayerLockResult::already_running ? 0 : 1;
    }
    boot_stage_stderr("player-lock-acquired");
    boot_stage_stderr("legacy-migration-begin");
    migrate_legacy_library_database();
    boot_stage_stderr("legacy-migration-complete");
    bfplayer::diagnostics_init(argc, argv);
    boot_stage("diagnostics-ready");
    bfplayer::diagnostics_install_ffmpeg();
    SDL_LogSetOutputFunction(sdl_log_output, nullptr);
    log_installed_manifest(argc > 0 ? argv[0] : nullptr);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "application-start build=%s player_lock=acquired",
        BFPLAYER_VERSION);
    App app{};
    app.fallback_font = executable_asset_path(
        argc > 0 ? argv[0] : nullptr,
        "assets/fonts/NotoSans-Regular.ttf");
    app.ui_logo = executable_asset_path(
        argc > 0 ? argv[0] : nullptr,
        "sce_sys/icon0.png");
    boot_stage("sdl-init-begin");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        log_sdl("SDL_Init");
        bfplayer::diagnostics_shutdown();
        return 1;
    }
    boot_stage("sdl-init-complete");
    Kit_SetHint(KIT_HINT_THREAD_COUNT, kVideoDecoderThreads);
    apply_playback_buffer_policy(playback_buffer_policy(false));
    boot_stage("kitchensink-init-begin");
    if (Kit_Init(KIT_INIT_NETWORK | KIT_INIT_ASS) != 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "kitchensink-init failed error=%s",
            Kit_GetError());
        SDL_Quit();
        bfplayer::diagnostics_shutdown();
        return 1;
    }
    boot_stage("kitchensink-init-complete");
    Kit_Version kitchensink_version{};
    Kit_GetVersion(&kitchensink_version);
    SDL_version sdl_version{};
    SDL_GetVersion(&sdl_version);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "libraries sdl=%u.%u.%u kitchensink=%u.%u.%u cpu_count=%d decoder_threads=%d video_packet_buffers=%d video_frame_buffers=%d",
        sdl_version.major,
        sdl_version.minor,
        sdl_version.patch,
        kitchensink_version.major,
        kitchensink_version.minor,
        kitchensink_version.patch,
        SDL_GetCPUCount(),
        Kit_GetHint(KIT_HINT_THREAD_COUNT),
        Kit_GetHint(KIT_HINT_VIDEO_BUFFER_PACKETS),
        Kit_GetHint(KIT_HINT_VIDEO_BUFFER_FRAMES));

    boot_stage("window-create-begin");
    app.window = SDL_CreateWindow(
        "BFplayer",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        kWindowWidth,
        kWindowHeight,
        0);
    if (!app.window) {
        log_sdl("SDL_CreateWindow");
        cleanup(app);
        bfplayer::diagnostics_shutdown();
        return 1;
    }
    if (!configure_fullscreen_output(app)) {
        cleanup(app);
        bfplayer::diagnostics_shutdown();
        return 1;
    }
    boot_stage("window-create-complete");
    boot_stage("renderer-create-begin");
    app.renderer = create_renderer(app.window);
    if (!app.renderer) {
        log_sdl("SDL_CreateRenderer");
        cleanup(app);
        bfplayer::diagnostics_shutdown();
        return 1;
    }
    if (SDL_RenderSetLogicalSize(
            app.renderer,
            kWindowWidth,
            kWindowHeight) != 0) {
        log_sdl("SDL_RenderSetLogicalSize");
        cleanup(app);
        bfplayer::diagnostics_shutdown();
        return 1;
    }
    log_display_output(app);
    boot_stage("renderer-create-complete");
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            app.controller = SDL_GameControllerOpen(i);
            break;
        }
    }
    boot_stage("controller-open-complete");
    std::string remote_error;
    if (!app.remote_control.start(
            kRemoteControlPort,
            BFPLAYER_VERSION,
            remote_error)) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "remote-control start failed port=%u error=%s",
            static_cast<unsigned int>(kRemoteControlPort),
            remote_error.c_str());
    }

    int result = 0;
    const char* media_path = nullptr;
    const char* subtitle_path = nullptr;
    std::vector<bfplayer::MediaSource> initial_sources;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--subtitle") == 0 && index + 1 < argc) {
            subtitle_path = argv[++index];
        } else if (std::strcmp(argv[index], "--add-movie") == 0 &&
                   index + 1 < argc) {
            const char* source_path = argv[++index];
            initial_sources.push_back({
                bfplayer::MediaSourceKind::movie_file,
                source_path ? source_path : "",
                {}});
        } else if (std::strcmp(argv[index], "--add-tv-folder") == 0 &&
                   index + 1 < argc) {
            const char* source_path = argv[++index];
            initial_sources.push_back({
                bfplayer::MediaSourceKind::tv_folder,
                source_path ? source_path : "",
                {}});
        } else if (!media_path && argv[index][0]) {
            media_path = argv[index];
        }
    }
    if (media_path) {
        boot_stage("player-loop-enter");
        result = run_player(app, media_path, subtitle_path) == PlaybackOutcome::error ? 1 : 0;
    } else {
        boot_stage("library-loop-enter");
        result = run_library(app, initial_sources);
    }

    cleanup(app);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "application-end result=%d",
        result);
    return_to_playstation_home();
    bfplayer::diagnostics_shutdown();
    return result;
}
