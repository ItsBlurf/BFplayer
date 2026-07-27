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

#include "ps5mc/external_subtitles.hpp"
#include "ps5mc/controller_buttons.hpp"
#include "ps5mc/diagnostics.hpp"
#include "ps5mc/library_database.hpp"
#include "ps5mc/library_scanner.hpp"
#include "ps5mc/library_ui.hpp"
#include "ps5mc/kitchensink_subtitle_timing.h"
#include "ps5mc/playlist.hpp"
#include "ps5mc/playback_osd.hpp"
#include "ps5mc/player_settings.hpp"
#include "ps5mc/safe_read_file.hpp"
#include "ps5mc/source_uri.hpp"
#include "ps5mc/video_layout.hpp"

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

#ifndef PS5MC_VERSION
#define PS5MC_VERSION "development"
#endif

constexpr int kWindowWidth = 1920;
constexpr int kWindowHeight = 1080;
constexpr int kAudioBufferBytes = 64 * 1024;
constexpr int kSubtitleAtlasSize = 4096;
constexpr int kSubtitleFragments = 1024;
constexpr int kVideoDecoderThreads = 16;
constexpr int kVideoPacketBufferCount = 64;
constexpr int kVideoFrameBufferCount = 3;

enum class PlaybackOverlay {
    none,
    menu,
    controls,
    settings,
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
    ps5mc::ExternalSubtitles external_subtitles;
    ps5mc::SafeReadFile local_media;
    ps5mc::PlaybackOsd osd;
    std::vector<std::string> subtitle_sidecars;
    std::string current_media_path;
    std::string fallback_font;
    std::string ui_logo;
    int external_subtitle_index = -1;
    int volume_percent = 100;
    int previous_volume_percent = 100;
    std::int64_t subtitle_delay_ms = 0;
    ps5mc::PlayerSettings settings;
    PlaybackOverlay playback_overlay = PlaybackOverlay::none;
    int playback_overlay_selected = 0;
    ps5mc::VideoScaleMode video_scale_mode = ps5mc::VideoScaleMode::fit;
    ps5mc::VideoAspectMode video_aspect_mode =
        ps5mc::VideoAspectMode::default_ratio;
    ps5mc::VideoCropMode video_crop_mode =
        ps5mc::VideoCropMode::default_crop;
    bool running = true;
    bool playback_running = false;
    bool paused = false;
    std::atomic<bool> io_cancel{false};
    std::atomic<std::int64_t> source_open_deadline_ms{0};
};

enum class PlaybackOutcome {
    finished,
    user_return,
    error,
};

struct VideoSourceInfo {
    const char* codec = "none";
    const char* pixel_format = "unknown";
    int profile = -1;
    int width = 0;
    int height = 0;
    int bit_depth = 0;
    double frame_rate = 0.0;
    std::int64_t bit_rate = 0;
    bool demanding_software_decode = false;
};

void close_media(App& app);

void log_sdl(const char* operation) {
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::error,
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
    info.bit_rate = parameters->bit_rate > 0
        ? parameters->bit_rate
        : format->bit_rate;
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
    const VideoSourceInfo& source) {
    Kit_VideoFormatRequest request{};
    Kit_ResetVideoFormatRequest(&request);
    if (source.width < 1 || source.height < 1 ||
        (source.width <= kWindowWidth && source.height <= kWindowHeight)) {
        return request;
    }
    const double scale = std::min(
        static_cast<double>(kWindowWidth) / source.width,
        static_cast<double>(kWindowHeight) / source.height);
    request.width = std::max(
        2,
        static_cast<int>(std::floor(source.width * scale)) & ~1);
    request.height = std::max(
        2,
        static_cast<int>(std::floor(source.height * scale)) & ~1);
    return request;
}

void sdl_log_output(
    void*,
    int category,
    SDL_LogPriority priority,
    const char* message) {
    const ps5mc::DiagnosticLevel level =
        priority >= SDL_LOG_PRIORITY_ERROR
            ? ps5mc::DiagnosticLevel::error
            : (priority == SDL_LOG_PRIORITY_WARN
                   ? ps5mc::DiagnosticLevel::warning
                   : ps5mc::DiagnosticLevel::info);
    ps5mc::diagnostics_log(
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
        "/data/PS5-MediaCenter/library.db";
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
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::warning,
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
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "build-manifest bytes=%zu complete=%d content=%s",
        bytes,
        complete ? 1 : 0,
        contents);
}

ps5mc::PlayerSettings load_player_settings(
    ps5mc::LibraryDatabase& database) {
    ps5mc::PlayerSettings settings{};
    std::string value;
    int integer = 0;
    bool boolean = false;
    if (database.get_setting(
            std::string(ps5mc::kSettingVolumePercent),
            value) &&
        ps5mc::parse_setting_integer(value, 0, 100, integer)) {
        settings.volume_percent = integer;
    }
    if (database.get_setting(
            std::string(ps5mc::kSettingShortSeekSeconds),
            value) &&
        ps5mc::parse_setting_integer(value, 1, 300, integer)) {
        settings.short_seek_seconds = integer;
    }
    if (database.get_setting(
            std::string(ps5mc::kSettingLongSeekSeconds),
            value) &&
        ps5mc::parse_setting_integer(value, 1, 900, integer)) {
        settings.long_seek_seconds = integer;
    }
    if (database.get_setting(
            std::string(ps5mc::kSettingOsdDurationMs),
            value) &&
        ps5mc::parse_setting_integer(value, 500, 30000, integer)) {
        settings.osd_duration_ms = integer;
    }
    if (database.get_setting(
            std::string(ps5mc::kSettingResumePlayback),
            value) &&
        ps5mc::parse_setting_boolean(value, boolean)) {
        settings.resume_playback = boolean;
    }
    if (database.get_setting(
            std::string(ps5mc::kSettingAutoSubtitles),
            value) &&
        ps5mc::parse_setting_boolean(value, boolean)) {
        settings.auto_subtitles = boolean;
    }
    return ps5mc::normalized_player_settings(settings);
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
    auto* file = static_cast<ps5mc::SafeReadFile*>(opaque);
    if (!file) {
        return AVERROR(EINVAL);
    }
    const int result = file->read(buffer, length);
    return result == 0 ? AVERROR_EOF : result;
}

std::int64_t seek_local_media(void* opaque, std::int64_t offset, int whence) {
    auto* file = static_cast<ps5mc::SafeReadFile*>(opaque);
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
        ps5mc::diagnostics_log(ps5mc::DiagnosticLevel::error, "source-open rejected reason=empty");
        return nullptr;
    }
    const bool network = ps5mc::is_network_uri(path);
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "source-open begin kind=%s path=%s",
        network ? "network" : "local",
        ps5mc::redact_uri_secrets(path).c_str());
    if (network && !ps5mc::is_supported_stream_uri(path)) {
        error = "Unsupported network protocol";
        ps5mc::diagnostics_log(ps5mc::DiagnosticLevel::error, "source-open rejected reason=unsupported-protocol");
        return nullptr;
    }
    if (network && ps5mc::uri_has_credentials(path)) {
        error = "Network URLs containing usernames or passwords are rejected";
        ps5mc::diagnostics_log(ps5mc::DiagnosticLevel::error, "source-open rejected reason=credentials");
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
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
            "source-open failed path=%s ffmpeg=%d error=%s",
            ps5mc::redact_uri_secrets(path).c_str(),
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
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "source-open success path=%s streams=%u duration_us=%lld format=%s",
        ps5mc::redact_uri_secrets(path).c_str(),
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

    if (Kit_GetPlayerSubtitleStream(app.player) >= 0) {
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
    }
    return true;
}

double player_display_aspect(
    App& app,
    int frame_width,
    int frame_height) {
    int numerator = 0;
    int denominator = 0;
    Kit_GetPlayerAspectRatio(app.player, &numerator, &denominator);
    return ps5mc::display_aspect_from_sample_aspect(
        frame_width,
        frame_height,
        numerator,
        denominator);
}

ps5mc::VideoLayout player_video_layout(App& app) {
    Kit_PlayerInfo info{};
    Kit_GetPlayerInfo(app.player, &info);
    return ps5mc::compute_video_layout(
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
    if (previous == index) {
        return true;
    }
    if (Kit_SetPlayerStream(app.player, type, index) != 0) {
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
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
    } else if (type == KIT_STREAMTYPE_VIDEO || type == KIT_STREAMTYPE_SUBTITLE) {
        resources_ready = create_video_textures(app);
    }
    if (!resources_ready) {
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
            "track-switch resource-failure type=%d requested=%d restoring=%d",
            type,
            index,
            previous);
        const bool decoder_restored =
            Kit_SetPlayerStream(app.player, type, previous) == 0;
        const bool resources_restored = decoder_restored &&
            (type == KIT_STREAMTYPE_AUDIO
                 ? open_audio(app)
                 : create_video_textures(app));
        if (!resources_restored) {
            ps5mc::diagnostics_log(
                ps5mc::DiagnosticLevel::error,
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

    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
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
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::warning,
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
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
            "subtitle-open failed path=%s error=%s",
            ps5mc::redact_uri_secrets(
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
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "subtitle-open success index=%d path=%s",
        index,
        ps5mc::redact_uri_secrets(
            app.subtitle_sidecars[static_cast<std::size_t>(index)]).c_str());
    app.osd.show(
        "Subtitles: " + ps5mc::redact_uri_secrets(
            app.subtitle_sidecars[static_cast<std::size_t>(index)]));
    return true;
}

ps5mc::TrackPreferences current_track_preferences(const App& app) {
    ps5mc::TrackPreferences preferences{};
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
    const ps5mc::TrackPreferences& preferences,
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
        ps5mc::diagnostics_log(ps5mc::DiagnosticLevel::info, "subtitle-selection off");
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
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
            "seek failed from=%.3f delta=%.3f target=%.3f error=%s",
            position,
            seconds,
            target,
            Kit_GetError());
    } else if (app.audio != 0) {
        SDL_ClearQueuedAudio(app.audio);
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::info,
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
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::info,
            "chapter-seek success index=%d target=%.3f",
            target_index,
            target);
    } else {
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
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
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
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
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
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
    app.paused = !app.paused;
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
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
}

void cycle_video_scale(App& app) {
    app.video_scale_mode = ps5mc::next_video_scale_mode(app.video_scale_mode);
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "video-scale mode=%s",
        ps5mc::video_scale_mode_name(app.video_scale_mode));
    app.osd.show(
        std::string("Video scale: ") +
        ps5mc::video_scale_mode_name(app.video_scale_mode));
}

void cycle_video_aspect(App& app) {
    app.video_aspect_mode =
        ps5mc::next_video_aspect_mode(app.video_aspect_mode);
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "video-aspect mode=%s",
        ps5mc::video_aspect_mode_name(app.video_aspect_mode));
    app.osd.show(
        std::string("Aspect ratio: ") +
        ps5mc::video_aspect_mode_name(app.video_aspect_mode));
}

void cycle_video_crop(App& app) {
    app.video_crop_mode =
        ps5mc::next_video_crop_mode(app.video_crop_mode);
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "video-crop mode=%s",
        ps5mc::video_crop_mode_name(app.video_crop_mode));
    app.osd.show(
        std::string("Crop ratio: ") +
        ps5mc::video_crop_mode_name(app.video_crop_mode));
}

bool persist_active_player_settings(App& app) {
    const ps5mc::PlayerSettings settings =
        ps5mc::normalized_player_settings(app.settings);
    ps5mc::LibraryDatabase database;
    const bool saved =
        database.open("/data/BFplayer/library.db") &&
        database.set_settings({
            {std::string(ps5mc::kSettingVolumePercent),
             std::to_string(settings.volume_percent)},
            {std::string(ps5mc::kSettingShortSeekSeconds),
             std::to_string(settings.short_seek_seconds)},
            {std::string(ps5mc::kSettingLongSeekSeconds),
             std::to_string(settings.long_seek_seconds)},
            {std::string(ps5mc::kSettingOsdDurationMs),
             std::to_string(settings.osd_duration_ms)},
            {std::string(ps5mc::kSettingResumePlayback),
             settings.resume_playback ? "1" : "0"},
            {std::string(ps5mc::kSettingAutoSubtitles),
             settings.auto_subtitles ? "1" : "0"},
            {"video_scale_mode",
             ps5mc::video_scale_mode_key(app.video_scale_mode)},
            {"video_crop_mode",
             ps5mc::video_crop_mode_key(app.video_crop_mode)},
        });
    if (!saved) {
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
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
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "player-settings live-save volume=%d short_seek=%d long_seek=%d osd_ms=%d resume=%d auto_subtitles=%d scale=%s aspect=%s crop=%s",
        settings.volume_percent,
        settings.short_seek_seconds,
        settings.long_seek_seconds,
        settings.osd_duration_ms,
        settings.resume_playback ? 1 : 0,
        settings.auto_subtitles ? 1 : 0,
        ps5mc::video_scale_mode_name(app.video_scale_mode),
        ps5mc::video_aspect_mode_name(app.video_aspect_mode),
        ps5mc::video_crop_mode_name(app.video_crop_mode));
    return true;
}

void refresh_playback_overlay(App& app) {
    if (app.playback_overlay == PlaybackOverlay::none) {
        app.osd.hide_panel();
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
                "Circle            Change subtitle track",
                "Square            Change audio track",
                "Triangle          Change video track",
                "L3 / R3           Volume down / up",
                "L2 + Triangle     Crop mode",
                "R2 + Triangle     Aspect ratio",
                "L2+R2+Triangle    Scale mode",
                "Touchpad          Show this controls page",
                "Options           Menu, mute, settings, return",
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
                    ps5mc::video_scale_mode_name(app.video_scale_mode),
                std::string("Display aspect (this video)     ") +
                    ps5mc::video_aspect_mode_name(app.video_aspect_mode),
                std::string("Crop                            ") +
                    ps5mc::video_crop_mode_name(app.video_crop_mode),
                std::string("Resume where I stopped         ") +
                    (app.settings.resume_playback ? "On" : "Off"),
                std::string("Automatically select subtitles  ") +
                    (app.settings.auto_subtitles ? "On" : "Off"),
                "Restore playback defaults",
            },
            app.playback_overlay_selected);
        return;
    }
    app.osd.show_panel(
        "Playback menu",
        {
            "Resume playback",
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
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "playback-overlay open=%s",
        overlay == PlaybackOverlay::menu
            ? "menu"
            : (overlay == PlaybackOverlay::controls
                   ? "controls"
                   : "settings"));
}

bool handle_playback_overlay_button(
    App& app,
    SDL_GameControllerButton button) {
    if (app.playback_overlay == PlaybackOverlay::none) {
        return false;
    }
    if (button == ps5mc::kControllerOptionsButton) {
        app.playback_overlay = PlaybackOverlay::none;
        refresh_playback_overlay(app);
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
                (app.playback_overlay_selected + 10 + direction) % 10;
            refresh_playback_overlay(app);
            return true;
        }
        if (button == SDL_CONTROLLER_BUTTON_X) {
            const ps5mc::PlayerSettings previous = app.settings;
            const ps5mc::VideoScaleMode previous_scale =
                app.video_scale_mode;
            const ps5mc::VideoAspectMode previous_aspect =
                app.video_aspect_mode;
            const ps5mc::VideoCropMode previous_crop =
                app.video_crop_mode;
            app.settings = {};
            app.video_scale_mode = ps5mc::VideoScaleMode::fit;
            app.video_aspect_mode =
                ps5mc::VideoAspectMode::default_ratio;
            app.video_crop_mode =
                ps5mc::VideoCropMode::default_crop;
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
        const int direction =
            button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : 1;
        const ps5mc::PlayerSettings previous = app.settings;
        const ps5mc::VideoScaleMode previous_scale =
            app.video_scale_mode;
        const ps5mc::VideoAspectMode previous_aspect =
            app.video_aspect_mode;
        const ps5mc::VideoCropMode previous_crop =
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
                    ps5mc::next_short_seek_seconds(
                        app.settings.short_seek_seconds,
                        direction);
                break;
            case 2:
                app.settings.long_seek_seconds =
                    ps5mc::next_long_seek_seconds(
                        app.settings.long_seek_seconds,
                        direction);
                break;
            case 3:
                app.settings.osd_duration_ms =
                    ps5mc::next_osd_duration_ms(
                        app.settings.osd_duration_ms,
                        direction);
                break;
            case 4:
                app.video_scale_mode =
                    ps5mc::step_video_scale_mode(
                        app.video_scale_mode,
                        direction);
                break;
            case 5:
                app.video_aspect_mode =
                    ps5mc::step_video_aspect_mode(
                        app.video_aspect_mode,
                        direction);
                break;
            case 6:
                app.video_crop_mode =
                    ps5mc::step_video_crop_mode(
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
            case 9:
                app.settings = {};
                app.video_scale_mode = ps5mc::VideoScaleMode::fit;
                app.video_aspect_mode =
                    ps5mc::VideoAspectMode::default_ratio;
                app.video_crop_mode =
                    ps5mc::VideoCropMode::default_crop;
                break;
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
            (app.playback_overlay_selected + 6 + direction) % 6;
        refresh_playback_overlay(app);
        return true;
    }
    if (app.playback_overlay_selected == 4 &&
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
            open_playback_overlay(app, PlaybackOverlay::controls);
            break;
        case 2:
            open_playback_overlay(app, PlaybackOverlay::settings);
            break;
        case 3:
            toggle_mute(app);
            refresh_playback_overlay(app);
            break;
        case 4:
            app.subtitle_delay_ms = 0;
            app.osd.show("Subtitle timing reset to 0 ms");
            refresh_playback_overlay(app);
            break;
        case 5:
            app.playback_running = false;
            ps5mc::diagnostics_log(
                ps5mc::DiagnosticLevel::info,
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
        case ps5mc::kControllerTouchpadButton:
            open_playback_overlay(app, PlaybackOverlay::controls);
            break;
#if !defined(PS5MC_PS5)
        case SDL_CONTROLLER_BUTTON_BACK:
            toggle_mute(app);
            break;
#endif
        case ps5mc::kControllerOptionsButton:
            open_playback_overlay(app, PlaybackOverlay::menu);
            break;
        default:
            break;
    }
}

void pump_events(App& app) {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                app.running = false;
                app.playback_running = false;
                break;
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
                on_controller_button(
                    app, static_cast<SDL_GameControllerButton>(event.cbutton.button));
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
}

PlaybackOutcome run_player(
    App& app,
    const char* path,
    const char* explicit_subtitle = nullptr) {
    // Aspect overrides are playback-local. Each newly opened video starts by
    // honoring its reported display aspect, with frame dimensions as fallback.
    app.video_aspect_mode = ps5mc::VideoAspectMode::default_ratio;
    app.current_media_path = path ? ps5mc::redact_uri_secrets(path) : std::string{};
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "playback-start path=%s explicit_subtitle=%s",
        app.current_media_path.c_str(),
        explicit_subtitle && explicit_subtitle[0]
            ? ps5mc::redact_uri_secrets(explicit_subtitle).c_str()
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
                         ps5mc::kControllerOptionsButton)) {
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
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
            "playback-start failed path=%s error=%s",
            app.current_media_path.c_str(),
            source_error.c_str());
        show_open_error(source_error);
        return PlaybackOutcome::error;
    }
    ps5mc::LibraryDatabase resume_database;
    const bool database_available =
        resume_database.open("/data/BFplayer/library.db");
    app.settings = database_available
        ? load_player_settings(resume_database)
        : ps5mc::PlayerSettings{};
    app.volume_percent = app.settings.volume_percent;
    app.previous_volume_percent =
        app.volume_percent > 0 ? app.volume_percent : 100;
    app.osd.set_default_duration(
        static_cast<std::uint64_t>(app.settings.osd_duration_ms));
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "player-settings volume=%d short_seek=%d long_seek=%d osd_ms=%d resume=%d auto_subtitles=%d",
        app.settings.volume_percent,
        app.settings.short_seek_seconds,
        app.settings.long_seek_seconds,
        app.settings.osd_duration_ms,
        app.settings.resume_playback ? 1 : 0,
        app.settings.auto_subtitles ? 1 : 0);
    app.subtitle_sidecars = ps5mc::is_network_uri(path)
        ? std::vector<std::string>{}
        : ps5mc::find_subtitle_sidecars(path);
    if (explicit_subtitle && explicit_subtitle[0]) {
        app.subtitle_sidecars.insert(app.subtitle_sidecars.begin(), explicit_subtitle);
    }
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "subtitle-sidecars count=%zu",
        app.subtitle_sidecars.size());
    if (const auto* format = static_cast<const AVFormatContext*>(app.source->format_ctx)) {
        for (unsigned int index = 0; index < format->nb_streams; ++index) {
            const AVMediaType type = format->streams[index]->codecpar->codec_type;
            if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO ||
                type == AVMEDIA_TYPE_SUBTITLE) {
                ps5mc::diagnostics_log(
                    ps5mc::DiagnosticLevel::info,
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
    const VideoSourceInfo source_video = inspect_video_source(
        static_cast<const AVFormatContext*>(app.source->format_ctx),
        initial_video);
    const Kit_VideoFormatRequest video_request =
        make_video_format_request(source_video);
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "video-source codec=%s profile=%d pix_fmt=%s bit_depth=%d size=%dx%d fps=%.3f bitrate=%lld demanding_software_decode=%d",
        source_video.codec,
        source_video.profile,
        source_video.pixel_format,
        source_video.bit_depth,
        source_video.width,
        source_video.height,
        source_video.frame_rate,
        static_cast<long long>(source_video.bit_rate),
        source_video.demanding_software_decode ? 1 : 0);
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "video-output-request source=%dx%d output=%dx%d downscale=%d",
        source_video.width,
        source_video.height,
        video_request.width,
        video_request.height,
        video_request.width > 0 && video_request.height > 0 ? 1 : 0);
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
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
            "player-create failed error=%s",
            Kit_GetError());
        show_open_error(Kit_GetError());
        return PlaybackOutcome::error;
    }
    if (Kit_GetPlayerVideoStream(app.player) < 0 &&
        Kit_GetPlayerAudioStream(app.player) < 0) {
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
            "player-create failed reason=no-decodable-audio-video");
        return PlaybackOutcome::error;
    }

    if (!open_audio(app) || !create_video_textures(app)) {
        show_open_error(SDL_GetError());
        return PlaybackOutcome::error;
    }
    if (!opening_osd_ready) {
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::warning,
            "osd-open failed error=%s",
            app.osd.error().c_str());
    }

    const bool source_persistence_allowed =
        !ps5mc::uri_has_sensitive_components(path);
    ps5mc::ResumeState saved_resume{};
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
            ps5mc::diagnostics_log(
                ps5mc::DiagnosticLevel::info,
                "resume-pending seconds=%.3f duration=%.3f",
                resume_seconds,
                duration);
        } else {
            ps5mc::diagnostics_log(
                ps5mc::DiagnosticLevel::info,
                "resume-skipped saved_ms=%lld duration=%.3f completed=%d",
                static_cast<long long>(saved_resume.position_ms),
                duration,
                saved_resume.completed ? 1 : 0);
        }
    } else if (app.settings.resume_playback && database_available &&
               source_persistence_allowed) {
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::info,
            "resume-not-found");
    }
    std::string saved_video_scale;
    if (database_available &&
        resume_database.get_setting("video_scale_mode", saved_video_scale)) {
        const std::optional<ps5mc::VideoScaleMode> parsed =
            ps5mc::parse_video_scale_mode(saved_video_scale);
        if (parsed.has_value()) {
            app.video_scale_mode = *parsed;
        }
    }
    std::string saved_video_crop;
    if (database_available &&
        resume_database.get_setting("video_crop_mode", saved_video_crop)) {
        const std::optional<ps5mc::VideoCropMode> parsed =
            ps5mc::parse_video_crop_mode(saved_video_crop);
        if (parsed.has_value()) {
            app.video_crop_mode = *parsed;
        }
    }
    ps5mc::TrackPreferences saved_preferences{};
    if (database_available && source_persistence_allowed &&
        resume_database.load_track_preferences(path, saved_preferences)) {
        restore_track_preferences(
            app,
            saved_preferences,
            explicit_subtitle && explicit_subtitle[0]);
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::info,
            "preferences-restored audio=%d subtitle=%d external=%s delay_ms=%lld",
            saved_preferences.audio_stream,
            saved_preferences.subtitle_stream,
            ps5mc::redact_uri_secrets(saved_preferences.external_subtitle).c_str(),
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
        ps5mc::display_aspect_from_sample_aspect(
            info.video_format.width,
            info.video_format.height,
            sample_aspect_numerator,
            sample_aspect_denominator);
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "playback-format video=%s threads=%d output=%dx%d format=%u audio=%s threads=%d %dHz/%dch duration_s=%.3f",
        info.video_codec.name,
        info.video_codec.threads,
        info.video_format.width,
        info.video_format.height,
        static_cast<unsigned int>(info.video_format.format),
        info.audio_codec.name,
        info.audio_codec.threads,
        info.audio_format.sample_rate,
        info.audio_format.channels,
        Kit_GetPlayerDuration(app.player));
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
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
            ps5mc::diagnostics_log(
                ps5mc::DiagnosticLevel::info,
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
            ps5mc::diagnostics_log(
                ps5mc::DiagnosticLevel::error,
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
    std::uint64_t video_updates = 0;
    std::uint64_t video_empty_polls = 0;
    static_assert(kAudioBufferBytes % sizeof(std::int16_t) == 0);
    std::array<std::int16_t, kAudioBufferBytes / sizeof(std::int16_t)> audio_buffer{};
    std::array<SDL_Rect, kSubtitleFragments> subtitle_sources{};
    std::array<SDL_Rect, kSubtitleFragments> subtitle_targets{};

    while (app.running && app.playback_running &&
           Kit_GetPlayerState(app.player) != KIT_STOPPED) {
        pump_events(app);

        if (!app.paused && app.audio != 0) {
            const Uint32 queued = SDL_GetQueuedAudioSize(app.audio);
            if (queued < kAudioBufferBytes) {
                const std::size_t available =
                    static_cast<std::size_t>(kAudioBufferBytes - queued);
                const int got = Kit_GetPlayerAudioData(
                    app.player,
                    queued,
                    reinterpret_cast<unsigned char*>(audio_buffer.data()),
                    available);
                if (got > 0) {
                    if (static_cast<std::size_t>(got) > available ||
                        got % static_cast<int>(sizeof(std::int16_t)) != 0) {
                        ps5mc::diagnostics_log(
                            ps5mc::DiagnosticLevel::error,
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

        SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
        SDL_RenderClear(app.renderer);
        if (app.video) {
            if (Kit_GetPlayerVideoSDLTexture(
                    app.player, app.video, nullptr) == 0) {
                ++video_updates;
            } else {
                ++video_empty_polls;
            }
            const ps5mc::VideoLayout layout = player_video_layout(app);
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
            const int fragments = ps5mc_get_player_subtitle_texture_at(
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
            if (!app.external_subtitles.render(position_ms, app.subtitle_delay_ms)) {
                std::fprintf(
                    stderr, "External subtitle render: %s\n",
                    app.external_subtitles.error().c_str());
                ps5mc::diagnostics_log(
                    ps5mc::DiagnosticLevel::error,
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
        SDL_RenderPresent(app.renderer);

        const Uint64 now = SDL_GetTicks64();
        if (now - last_diagnostics >= 5000) {
            unsigned int video_frames_length = 0;
            unsigned int video_frames_capacity = 0;
            unsigned int video_packets_length = 0;
            unsigned int video_packets_capacity = 0;
            unsigned int audio_frames_length = 0;
            unsigned int audio_frames_capacity = 0;
            unsigned int audio_packets_length = 0;
            unsigned int audio_packets_capacity = 0;
            if (Kit_GetPlayerVideoStream(app.player) >= 0) {
                Kit_GetPlayerVideoBufferState(
                    app.player,
                    &video_frames_length,
                    &video_frames_capacity,
                    &video_packets_length,
                    &video_packets_capacity);
            }
            if (Kit_GetPlayerAudioStream(app.player) >= 0) {
                Kit_GetPlayerAudioBufferState(
                    app.player,
                    &audio_frames_length,
                    &audio_frames_capacity,
                    &audio_packets_length,
                    &audio_packets_capacity);
            }
            const double diagnostics_seconds =
                static_cast<double>(now - last_diagnostics) / 1000.0;
            const double video_update_rate = diagnostics_seconds > 0.0
                ? static_cast<double>(video_updates) / diagnostics_seconds
                : 0.0;
            ps5mc::diagnostics_log(
                ps5mc::DiagnosticLevel::info,
                "playback-heartbeat path=%s position_s=%.3f duration_s=%.3f paused=%d state=%d video_updates=%llu video_update_fps=%.2f video_empty_polls=%llu video_frames=%u/%u video_packets=%u/%u audio_frames=%u/%u audio_packets=%u/%u audio_queued=%u external_subtitle=%d delay_ms=%lld scale=%s aspect=%s crop=%s",
                app.current_media_path.c_str(),
                Kit_GetPlayerPosition(app.player),
                Kit_GetPlayerDuration(app.player),
                app.paused ? 1 : 0,
                Kit_GetPlayerState(app.player),
                static_cast<unsigned long long>(video_updates),
                video_update_rate,
                static_cast<unsigned long long>(video_empty_polls),
                video_frames_length,
                video_frames_capacity,
                video_packets_length,
                video_packets_capacity,
                audio_frames_length,
                audio_frames_capacity,
                audio_packets_length,
                audio_packets_capacity,
                app.audio != 0 ? SDL_GetQueuedAudioSize(app.audio) : 0U,
                app.external_subtitle_index,
                static_cast<long long>(app.subtitle_delay_ms),
                ps5mc::video_scale_mode_name(app.video_scale_mode),
                ps5mc::video_aspect_mode_name(app.video_aspect_mode),
                ps5mc::video_crop_mode_name(app.video_crop_mode));
            video_updates = 0;
            video_empty_polls = 0;
            last_diagnostics = now;
        }
        if (database_available && source_persistence_allowed &&
            now - last_resume_save >= 10000) {
            ps5mc::ResumeState current{};
            current.position_ms = seconds_to_milliseconds(
                Kit_GetPlayerPosition(app.player));
            current.duration_ms = seconds_to_milliseconds(
                Kit_GetPlayerDuration(app.player));
            current.last_played_unix = static_cast<std::int64_t>(std::time(nullptr));
            current.completed = playback_completed(
                current.position_ms, current.duration_ms);
            if (!resume_database.save_resume(path, current)) {
                ps5mc::diagnostics_log(
                    ps5mc::DiagnosticLevel::error,
                    "resume-save failed error=%s",
                    resume_database.error().c_str());
            }
            const ps5mc::TrackPreferences preferences = current_track_preferences(app);
            if (!ps5mc::uri_has_sensitive_components(preferences.external_subtitle) &&
                !resume_database.save_track_preferences(path, preferences)) {
                ps5mc::diagnostics_log(
                    ps5mc::DiagnosticLevel::error,
                    "track-preference-save failed error=%s",
                    resume_database.error().c_str());
            }
            last_resume_save = now;
        }
    }
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
            ps5mc::ResumeState current{};
            // Use the values captured before any cleanup/stop transition.
            // Some decoder backends reset their public clock when stopping.
            current.position_ms = seconds_to_milliseconds(final_position);
            current.duration_ms = seconds_to_milliseconds(final_duration);
            current.last_played_unix = static_cast<std::int64_t>(std::time(nullptr));
            current.completed = playback_completed(
                current.position_ms, current.duration_ms);
            if (!resume_database.save_resume(path, current)) {
                ps5mc::diagnostics_log(
                    ps5mc::DiagnosticLevel::error,
                    "final-resume-save failed error=%s",
                    resume_database.error().c_str());
            }
            const ps5mc::TrackPreferences preferences = current_track_preferences(app);
            if (!ps5mc::uri_has_sensitive_components(
                    preferences.external_subtitle) &&
                !resume_database.save_track_preferences(path, preferences)) {
                ps5mc::diagnostics_log(
                    ps5mc::DiagnosticLevel::error,
                    "final-track-preference-save failed error=%s",
                    resume_database.error().c_str());
            }
        }
        if (!resume_database.set_settings({
                {"volume_percent", std::to_string(app.volume_percent)},
                {"video_scale_mode",
                 ps5mc::video_scale_mode_key(app.video_scale_mode)},
                {"video_crop_mode",
                 ps5mc::video_crop_mode_key(app.video_crop_mode)},
            })) {
            ps5mc::diagnostics_log(
                ps5mc::DiagnosticLevel::error,
                "playback-setting-save failed error=%s",
                resume_database.error().c_str());
        }
    }
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
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
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::info,
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
    app.current_media_path.clear();
    app.external_subtitle_index = -1;
    app.playback_running = false;
    app.paused = false;
    app.playback_overlay = PlaybackOverlay::none;
    app.playback_overlay_selected = 0;
}

int run_library(
    App& app,
    const std::vector<ps5mc::MediaSource>& initial_sources = {}) {
    ps5mc::diagnostics_log(ps5mc::DiagnosticLevel::info, "library-start");
    ps5mc::LibraryUi library;
    if (!library.open(
            app.renderer,
            app.fallback_font,
            app.ui_logo,
            initial_sources)) {
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
            "library-open failed error=%s",
            library.error().c_str());
        return 1;
    }

    while (app.running) {
        SDL_Event event{};
        bool play_selected = false;
        bool play_queue = false;
        std::string selected_path;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_CONTROLLERDEVICEADDED &&
                !app.controller &&
                SDL_IsGameController(event.cdevice.which)) {
                app.controller =
                    SDL_GameControllerOpen(event.cdevice.which);
                ps5mc::diagnostics_log(
                    app.controller
                        ? ps5mc::DiagnosticLevel::info
                        : ps5mc::DiagnosticLevel::warning,
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
                ps5mc::diagnostics_log(
                    ps5mc::DiagnosticLevel::info,
                    "library-controller removed id=%d",
                    event.cdevice.which);
            }
            const ps5mc::LibraryAction action = library.handle_event(event, selected_path);
            if (action == ps5mc::LibraryAction::exit) {
                app.running = false;
                break;
            }
            if (action == ps5mc::LibraryAction::play) {
                play_selected = true;
                break;
            }
            if (action == ps5mc::LibraryAction::play_queue) {
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
            if (ps5mc::is_generic_playlist_path(selected_path)) {
                ps5mc::PlaylistLoadResult playlist =
                    ps5mc::load_generic_playlist(selected_path);
                if (playlist.items.empty()) {
                    const std::string error = playlist.error.empty()
                        ? "Unsupported playlist"
                        : playlist.error;
                    ps5mc::diagnostics_log(
                        ps5mc::DiagnosticLevel::error,
                        "playlist-load failed path=%s error=%s",
                        selected_path.c_str(),
                        error.c_str());
                    library.show_notice("PLAYLIST: " + error);
                    continue;
                }
                if (playlist.truncated) {
                    ps5mc::diagnostics_log(
                        ps5mc::DiagnosticLevel::warning,
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
            ps5mc::diagnostics_log(
                ps5mc::DiagnosticLevel::info,
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
            if (app.running && !library.open(
                    app.renderer,
                    app.fallback_font,
                    app.ui_logo)) {
                ps5mc::diagnostics_log(
                    ps5mc::DiagnosticLevel::error,
                    "library-reopen failed error=%s",
                    library.error().c_str());
                app.running = false;
                break;
            }
            continue;
        }
        library.render();
        SDL_Delay(8);
    }
    library.close();
    ps5mc::diagnostics_log(ps5mc::DiagnosticLevel::info, "library-end running=%d", app.running ? 1 : 0);
    return 0;
}

void cleanup(App& app) {
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
    constexpr const char* kMediaCenterTitleId = "PSMC00001";
    const int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
    if (app_id <= 0) {
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
            "home-return failed reason=no-running-bigapp app_id=%d",
            app_id);
        return;
    }
    std::array<char, 16> title_id{};
    const int title_result =
        sceSystemServiceGetAppTitleId(app_id, title_id.data());
    if (title_result != 0) {
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
            "home-return failed reason=title-id app_id=%d result=0x%08x",
            app_id,
            static_cast<unsigned int>(title_result));
        return;
    }
    if (std::strcmp(title_id.data(), kMediaCenterTitleId) != 0) {
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
            "home-return skipped reason=unexpected-bigapp app_id=%d title_id=%s",
            app_id,
            title_id.data());
        return;
    }
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "home-return kill-bigapp app_id=%d title_id=%s",
        app_id,
        title_id.data());
    const int result =
        sceSystemServiceKillApp(app_id, -1, 0, 0);
    if (result != 0) {
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
            "home-return kill-bigapp failed app_id=%d result=0x%08x",
            app_id,
            static_cast<unsigned int>(result));
    }
}

} // namespace

int main(int argc, char** argv) {
    migrate_legacy_library_database();
    ps5mc::diagnostics_init(argc, argv);
    ps5mc::diagnostics_install_ffmpeg();
    SDL_LogSetOutputFunction(sdl_log_output, nullptr);
    log_installed_manifest(argc > 0 ? argv[0] : nullptr);
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "application-start build=%s",
        PS5MC_VERSION);
    App app{};
    app.fallback_font = executable_asset_path(
        argc > 0 ? argv[0] : nullptr,
        "assets/fonts/NotoSans-Regular.ttf");
    app.ui_logo = executable_asset_path(
        argc > 0 ? argv[0] : nullptr,
        "sce_sys/icon0.png");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        log_sdl("SDL_Init");
        ps5mc::diagnostics_shutdown();
        return 1;
    }
    Kit_SetHint(KIT_HINT_THREAD_COUNT, kVideoDecoderThreads);
    Kit_SetHint(KIT_HINT_VIDEO_BUFFER_PACKETS, kVideoPacketBufferCount);
    Kit_SetHint(KIT_HINT_VIDEO_BUFFER_FRAMES, kVideoFrameBufferCount);
    if (Kit_Init(KIT_INIT_NETWORK | KIT_INIT_ASS) != 0) {
        ps5mc::diagnostics_log(
            ps5mc::DiagnosticLevel::error,
            "kitchensink-init failed error=%s",
            Kit_GetError());
        SDL_Quit();
        ps5mc::diagnostics_shutdown();
        return 1;
    }
    Kit_Version kitchensink_version{};
    Kit_GetVersion(&kitchensink_version);
    SDL_version sdl_version{};
    SDL_GetVersion(&sdl_version);
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
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

    app.window = SDL_CreateWindow(
        "BFplayer",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        kWindowWidth,
        kWindowHeight,
        SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!app.window) {
        log_sdl("SDL_CreateWindow");
        cleanup(app);
        ps5mc::diagnostics_shutdown();
        return 1;
    }
    app.renderer = create_renderer(app.window);
    if (!app.renderer) {
        log_sdl("SDL_CreateRenderer");
        cleanup(app);
        ps5mc::diagnostics_shutdown();
        return 1;
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            app.controller = SDL_GameControllerOpen(i);
            break;
        }
    }

    int result = 0;
    const char* media_path = nullptr;
    const char* subtitle_path = nullptr;
    std::vector<ps5mc::MediaSource> initial_sources;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--subtitle") == 0 && index + 1 < argc) {
            subtitle_path = argv[++index];
        } else if (std::strcmp(argv[index], "--add-movie") == 0 &&
                   index + 1 < argc) {
            const char* source_path = argv[++index];
            initial_sources.push_back({
                ps5mc::MediaSourceKind::movie_file,
                source_path ? source_path : "",
                {}});
        } else if (std::strcmp(argv[index], "--add-tv-folder") == 0 &&
                   index + 1 < argc) {
            const char* source_path = argv[++index];
            initial_sources.push_back({
                ps5mc::MediaSourceKind::tv_folder,
                source_path ? source_path : "",
                {}});
        } else if (!media_path && argv[index][0]) {
            media_path = argv[index];
        }
    }
    if (media_path) {
        result = run_player(app, media_path, subtitle_path) == PlaybackOutcome::error ? 1 : 0;
    } else {
        result = run_library(app, initial_sources);
    }

    cleanup(app);
    ps5mc::diagnostics_log(
        ps5mc::DiagnosticLevel::info,
        "application-end result=%d",
        result);
    return_to_playstation_home();
    ps5mc::diagnostics_shutdown();
    return result;
}
