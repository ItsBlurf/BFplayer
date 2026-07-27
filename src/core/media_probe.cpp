#include "bfplayer/media_probe.hpp"

#include "bfplayer/safe_read_file.hpp"

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

namespace bfplayer {
namespace {

using Clock = std::chrono::steady_clock;

struct ProbeDeadline {
    Clock::time_point expires;
};

int interrupt_probe(void* opaque) {
    const auto* deadline = static_cast<const ProbeDeadline*>(opaque);
    return deadline && Clock::now() >= deadline->expires ? 1 : 0;
}

int read_probe_file(void* opaque, std::uint8_t* buffer, int length) {
    auto* file = static_cast<SafeReadFile*>(opaque);
    if (!file) {
        return AVERROR(EINVAL);
    }
    const int result = file->read(buffer, length);
    return result == 0 ? AVERROR_EOF : result;
}

std::int64_t seek_probe_file(void* opaque, std::int64_t offset, int whence) {
    auto* file = static_cast<SafeReadFile*>(opaque);
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

struct ProbeInput {
    SafeReadFile file;
    AVFormatContext* format = nullptr;
    AVIOContext* custom_io = nullptr;

    ~ProbeInput() {
        avformat_close_input(&format);
        if (custom_io) {
            av_freep(&custom_io->buffer);
            avio_context_free(&custom_io);
        }
    }
};

std::string ffmpeg_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(code, buffer, sizeof(buffer)) == 0) {
        return buffer;
    }
    return "FFmpeg error " + std::to_string(code);
}

std::string bounded_text(const char* value, std::size_t maximum = 512) {
    if (!value) {
        return {};
    }
    std::size_t length = 0;
    while (length < maximum && value[length] != '\0') {
        ++length;
    }
    return std::string(value, length);
}

std::int64_t stream_duration_ms(const AVStream* stream) {
    if (!stream || stream->duration == AV_NOPTS_VALUE || stream->duration <= 0) {
        return 0;
    }
    return av_rescale_q(stream->duration, stream->time_base, AVRational{1, 1000});
}

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

} // namespace

bool probe_media_metadata(
    MediaEntry& entry,
    std::string& error,
    int* fatal_io_error) {
    error.clear();
    if (fatal_io_error) {
        *fatal_io_error = 0;
    }
    ProbeInput input;
    if (!input.file.open(entry.path, error)) {
        if (fatal_io_error &&
            fatal_io_errno(input.file.last_error_code())) {
            *fatal_io_error = input.file.last_error_code();
        }
        error = "Open metadata file: " + error;
        return false;
    }
    input.format = avformat_alloc_context();
    AVFormatContext*& format = input.format;
    if (!format) {
        error = "Unable to allocate FFmpeg format context";
        return false;
    }

    ProbeDeadline deadline{Clock::now() + std::chrono::seconds(5)};
    format->interrupt_callback.callback = interrupt_probe;
    format->interrupt_callback.opaque = &deadline;

    constexpr int kAvioBufferBytes = 64 * 1024;
    auto* buffer = static_cast<std::uint8_t*>(av_malloc(kAvioBufferBytes));
    if (!buffer) {
        error = "Unable to allocate metadata I/O buffer";
        return false;
    }
    input.custom_io = avio_alloc_context(
        buffer,
        kAvioBufferBytes,
        0,
        &input.file,
        read_probe_file,
        nullptr,
        seek_probe_file);
    if (!input.custom_io) {
        av_free(buffer);
        error = "Unable to allocate metadata AVIO context";
        return false;
    }
    input.custom_io->seekable = AVIO_SEEKABLE_NORMAL;
    format->pb = input.custom_io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "probesize", "5242880", 0);
    av_dict_set(&options, "analyzeduration", "5000000", 0);
    av_dict_set(&options, "max_probe_packets", "10000", 0);
    av_dict_set(&options, "protocol_whitelist", "crypto,data,file", 0);
    int result =
        avformat_open_input(&format, entry.path.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (result < 0) {
        if (fatal_io_error &&
            fatal_io_errno(input.file.last_error_code())) {
            *fatal_io_error = input.file.last_error_code();
        }
        error = "Open metadata: " + ffmpeg_error(result);
        return false;
    }
    result = avformat_find_stream_info(format, nullptr);
    if (result < 0) {
        if (fatal_io_error &&
            fatal_io_errno(input.file.last_error_code())) {
            *fatal_io_error = input.file.last_error_code();
        }
        error = "Probe metadata: " + ffmpeg_error(result);
        return false;
    }

    entry.duration_ms = format->duration != AV_NOPTS_VALUE && format->duration > 0
        ? format->duration / (AV_TIME_BASE / 1000)
        : 0;
    entry.container = bounded_text(format->iformat ? format->iformat->name : nullptr, 128);
    if (const AVDictionaryEntry* title =
            av_dict_get(format->metadata, "title", nullptr, 0)) {
        entry.title = bounded_text(title->value);
    }

    const int video_index = av_find_best_stream(
        format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_index >= 0) {
        const AVStream* stream = format->streams[video_index];
        entry.width = std::max(0, stream->codecpar->width);
        entry.height = std::max(0, stream->codecpar->height);
        entry.video_codec = bounded_text(avcodec_get_name(stream->codecpar->codec_id), 64);
        if (entry.duration_ms <= 0) {
            entry.duration_ms = stream_duration_ms(stream);
        }
    }

    const int audio_index = av_find_best_stream(
        format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_index >= 0) {
        const AVStream* stream = format->streams[audio_index];
        entry.audio_codec = bounded_text(avcodec_get_name(stream->codecpar->codec_id), 64);
        if (entry.duration_ms <= 0) {
            entry.duration_ms = stream_duration_ms(stream);
        }
    }

    if (entry.container.empty()) {
        entry.container = "unknown";
    }
    return true;
}

} // namespace bfplayer
