#include "ps5mc/video_thumbnail.hpp"

#include "ps5mc/safe_read_file.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace ps5mc {
namespace {

using Clock = std::chrono::steady_clock;

struct ThumbnailDeadline {
    Clock::time_point expires;
    const std::atomic<bool>* cancelled = nullptr;
};

int interrupt_thumbnail(void* opaque) {
    const auto* deadline = static_cast<const ThumbnailDeadline*>(opaque);
    return deadline &&
            ((deadline->cancelled &&
              deadline->cancelled->load(std::memory_order_relaxed)) ||
             Clock::now() >= deadline->expires)
        ? 1
        : 0;
}

int read_thumbnail_file(void* opaque, std::uint8_t* buffer, int length) {
    auto* file = static_cast<SafeReadFile*>(opaque);
    if (!file) {
        return AVERROR(EINVAL);
    }
    const int result = file->read(buffer, length);
    return result == 0 ? AVERROR_EOF : result;
}

std::int64_t seek_thumbnail_file(
    void* opaque,
    std::int64_t offset,
    int whence) {
    auto* file = static_cast<SafeReadFile*>(opaque);
    if (!file) {
        return AVERROR(EINVAL);
    }
    if ((whence & AVSEEK_SIZE) == AVSEEK_SIZE) {
        return file->size() >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())
            ? AVERROR(EOVERFLOW)
            : static_cast<std::int64_t>(file->size());
    }
    const int origin = whence & ~AVSEEK_FORCE;
    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) {
        return AVERROR(EINVAL);
    }
    return file->seek(offset, origin);
}

std::string ffmpeg_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(code, buffer, sizeof(buffer)) == 0) {
        return buffer;
    }
    return "FFmpeg error " + std::to_string(code);
}

struct ThumbnailInput {
    SafeReadFile file;
    AVFormatContext* format = nullptr;
    AVIOContext* custom_io = nullptr;
    AVCodecContext* decoder = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;

    ~ThumbnailInput() {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        if (custom_io) {
            av_freep(&custom_io->buffer);
            avio_context_free(&custom_io);
        }
    }
};

int best_video_stream(const AVFormatContext* format) {
    if (!format) {
        return -1;
    }
    int best = -1;
    std::int64_t best_pixels = -1;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        const AVStream* stream = format->streams[index];
        if (!stream || !stream->codecpar ||
            stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
            (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
            continue;
        }
        const std::int64_t pixels =
            static_cast<std::int64_t>(std::max(0, stream->codecpar->width)) *
            static_cast<std::int64_t>(std::max(0, stream->codecpar->height));
        if (pixels > best_pixels) {
            best = static_cast<int>(index);
            best_pixels = pixels;
        }
    }
    return best;
}

std::int64_t duration_us(
    const AVFormatContext* format,
    const AVStream* stream) {
    if (format && format->duration != AV_NOPTS_VALUE &&
        format->duration > 0) {
        return format->duration;
    }
    if (stream && stream->duration != AV_NOPTS_VALUE &&
        stream->duration > 0) {
        return av_rescale_q(
            stream->duration,
            stream->time_base,
            AV_TIME_BASE_Q);
    }
    return 0;
}

double image_quality(const VideoThumbnail& thumbnail) {
    if (thumbnail.width <= 0 || thumbnail.height <= 0 ||
        thumbnail.rgba.size() <
            static_cast<std::size_t>(thumbnail.width) *
            static_cast<std::size_t>(thumbnail.height) * 4U) {
        return 0.0;
    }
    std::uint64_t sum = 0;
    std::uint8_t minimum = 255;
    std::uint8_t maximum = 0;
    std::size_t samples = 0;
    const int step_x = std::max(1, thumbnail.width / 64);
    const int step_y = std::max(1, thumbnail.height / 36);
    for (int y = 0; y < thumbnail.height; y += step_y) {
        for (int x = 0; x < thumbnail.width; x += step_x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * thumbnail.width + x) * 4U;
            const std::uint8_t luminance = static_cast<std::uint8_t>(
                (static_cast<unsigned int>(thumbnail.rgba[offset]) * 54U +
                 static_cast<unsigned int>(thumbnail.rgba[offset + 1U]) * 183U +
                 static_cast<unsigned int>(thumbnail.rgba[offset + 2U]) * 19U) >>
                8U);
            sum += luminance;
            minimum = std::min(minimum, luminance);
            maximum = std::max(maximum, luminance);
            ++samples;
        }
    }
    if (samples == 0) {
        return 0.0;
    }
    const double mean = static_cast<double>(sum) / samples;
    return mean + static_cast<double>(maximum - minimum) * 0.35;
}

bool convert_frame(
    const AVFrame* frame,
    std::int64_t position_ms,
    VideoThumbnail& output,
    std::string& error) {
    if (!frame || frame->width <= 0 || frame->height <= 0 ||
        frame->format < 0) {
        error = "Decoded thumbnail frame is invalid";
        return false;
    }
    constexpr int kMaximumWidth = 640;
    constexpr int kMaximumHeight = 360;
    const double scale = std::min({
        1.0,
        static_cast<double>(kMaximumWidth) / frame->width,
        static_cast<double>(kMaximumHeight) / frame->height});
    const int width = std::max(1, static_cast<int>(std::floor(frame->width * scale)));
    const int height =
        std::max(1, static_cast<int>(std::floor(frame->height * scale)));
    if (static_cast<std::uint64_t>(width) >
        std::numeric_limits<std::size_t>::max() /
            static_cast<std::uint64_t>(height) / 4U) {
        error = "Thumbnail dimensions overflow";
        return false;
    }
    VideoThumbnail candidate{};
    candidate.width = width;
    candidate.height = height;
    candidate.position_ms = position_ms;
    candidate.rgba.resize(
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height) * 4U);

    SwsContext* scaler = sws_getContext(
        frame->width,
        frame->height,
        static_cast<AVPixelFormat>(frame->format),
        width,
        height,
        AV_PIX_FMT_RGBA,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    if (!scaler) {
        error = "Unable to create thumbnail scaler";
        return false;
    }
    std::uint8_t* destination[4]{candidate.rgba.data(), nullptr, nullptr, nullptr};
    int destination_stride[4]{width * 4, 0, 0, 0};
    const int rows = sws_scale(
        scaler,
        frame->data,
        frame->linesize,
        0,
        frame->height,
        destination,
        destination_stride);
    sws_freeContext(scaler);
    if (rows != height) {
        error = "Thumbnail scaler returned incomplete output";
        return false;
    }
    output = std::move(candidate);
    return true;
}

bool decode_candidate(
    ThumbnailInput& input,
    int video_index,
    std::int64_t target_us,
    const ThumbnailDeadline& deadline,
    VideoThumbnail& output,
    std::string& error) {
    AVStream* stream = input.format->streams[video_index];
    const std::int64_t target_timestamp =
        av_rescale_q(target_us, AV_TIME_BASE_Q, stream->time_base);
    if (av_seek_frame(
            input.format,
            video_index,
            target_timestamp,
            AVSEEK_FLAG_BACKWARD) < 0) {
        error = "Unable to seek to thumbnail position";
        return false;
    }
    avformat_flush(input.format);
    avcodec_flush_buffers(input.decoder);
    av_packet_unref(input.packet);
    av_frame_unref(input.frame);

    int video_packets = 0;
    while (video_packets < 360 && !interrupt_thumbnail(
               const_cast<ThumbnailDeadline*>(&deadline))) {
        const int read_result = av_read_frame(input.format, input.packet);
        if (read_result < 0) {
            if (read_result != AVERROR_EOF) {
                error = "Read thumbnail packet: " +
                    ffmpeg_error(read_result);
            }
            break;
        }
        if (input.packet->stream_index != video_index) {
            av_packet_unref(input.packet);
            continue;
        }
        ++video_packets;
        int result = avcodec_send_packet(input.decoder, input.packet);
        av_packet_unref(input.packet);
        if (result < 0 && result != AVERROR(EAGAIN)) {
            error = "Send thumbnail packet: " + ffmpeg_error(result);
            return false;
        }
        while ((result = avcodec_receive_frame(
                    input.decoder,
                    input.frame)) >= 0) {
            const std::int64_t timestamp =
                input.frame->best_effort_timestamp;
            if (timestamp == AV_NOPTS_VALUE ||
                timestamp >= target_timestamp ||
                video_packets >= 120) {
                const std::int64_t position_ms =
                    timestamp == AV_NOPTS_VALUE
                    ? target_us / 1000
                    : av_rescale_q(
                          timestamp,
                          stream->time_base,
                          AVRational{1, 1000});
                return convert_frame(
                    input.frame,
                    position_ms,
                    output,
                    error);
            }
            av_frame_unref(input.frame);
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            error = "Decode thumbnail frame: " + ffmpeg_error(result);
            return false;
        }
    }
    if (interrupt_thumbnail(
            const_cast<ThumbnailDeadline*>(&deadline))) {
        error = "Thumbnail extraction cancelled or timed out";
    } else if (error.empty()) {
        error = "No video frame found near thumbnail position";
    }
    return false;
}

} // namespace

bool extract_video_thumbnail(
    const std::string& path,
    VideoThumbnail& output,
    std::string& error,
    const std::atomic<bool>* cancelled) {
    output = {};
    error.clear();
    ThumbnailInput input;
    if (!input.file.open(path, error)) {
        error = "Open thumbnail media: " + error;
        return false;
    }
    input.format = avformat_alloc_context();
    if (!input.format) {
        error = "Unable to allocate thumbnail format context";
        return false;
    }
    ThumbnailDeadline deadline{
        Clock::now() + std::chrono::seconds(8),
        cancelled};
    input.format->interrupt_callback.callback = interrupt_thumbnail;
    input.format->interrupt_callback.opaque = &deadline;

    constexpr int kIoBufferBytes = 64 * 1024;
    auto* buffer = static_cast<std::uint8_t*>(av_malloc(kIoBufferBytes));
    if (!buffer) {
        error = "Unable to allocate thumbnail I/O buffer";
        return false;
    }
    input.custom_io = avio_alloc_context(
        buffer,
        kIoBufferBytes,
        0,
        &input.file,
        read_thumbnail_file,
        nullptr,
        seek_thumbnail_file);
    if (!input.custom_io) {
        av_free(buffer);
        error = "Unable to allocate thumbnail AVIO context";
        return false;
    }
    input.custom_io->seekable = AVIO_SEEKABLE_NORMAL;
    input.format->pb = input.custom_io;
    input.format->flags |= AVFMT_FLAG_CUSTOM_IO;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "probesize", "5242880", 0);
    av_dict_set(&options, "analyzeduration", "5000000", 0);
    av_dict_set(&options, "max_probe_packets", "10000", 0);
    int result = avformat_open_input(
        &input.format,
        path.c_str(),
        nullptr,
        &options);
    av_dict_free(&options);
    if (result < 0) {
        error = "Open thumbnail container: " + ffmpeg_error(result);
        return false;
    }
    result = avformat_find_stream_info(input.format, nullptr);
    if (result < 0) {
        error = "Probe thumbnail container: " + ffmpeg_error(result);
        return false;
    }
    const int video_index = best_video_stream(input.format);
    if (video_index < 0) {
        error = "Media has no decodable video stream";
        return false;
    }
    AVStream* stream = input.format->streams[video_index];
    const AVCodec* codec =
        avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        error = "No decoder for thumbnail video stream";
        return false;
    }
    input.decoder = avcodec_alloc_context3(codec);
    if (!input.decoder ||
        avcodec_parameters_to_context(
            input.decoder,
            stream->codecpar) < 0 ||
        avcodec_open2(input.decoder, codec, nullptr) < 0) {
        error = "Unable to open thumbnail video decoder";
        return false;
    }
    input.frame = av_frame_alloc();
    input.packet = av_packet_alloc();
    if (!input.frame || !input.packet) {
        error = "Unable to allocate thumbnail decode buffers";
        return false;
    }

    const std::int64_t total_duration_us =
        duration_us(input.format, stream);
    const std::array<double, 3> fractions{0.50, 0.35, 0.65};
    const std::array<std::int64_t, 3> unknown_duration_targets{
        30 * AV_TIME_BASE,
        10 * AV_TIME_BASE,
        2 * AV_TIME_BASE};
    VideoThumbnail best{};
    double best_quality = -1.0;
    for (std::size_t index = 0; index < fractions.size(); ++index) {
        if (interrupt_thumbnail(&deadline)) {
            error = "Thumbnail extraction cancelled or timed out";
            break;
        }
        const std::int64_t target_us = total_duration_us > 0
            ? static_cast<std::int64_t>(
                  static_cast<long double>(total_duration_us) * fractions[index])
            : unknown_duration_targets[index];
        VideoThumbnail candidate{};
        std::string candidate_error;
        if (!decode_candidate(
                input,
                video_index,
                target_us,
                deadline,
                candidate,
                candidate_error)) {
            if (error.empty()) {
                error = std::move(candidate_error);
            }
            continue;
        }
        const double quality = image_quality(candidate);
        if (quality > best_quality) {
            best_quality = quality;
            best = std::move(candidate);
        }
        if (best_quality >= 38.0) {
            break;
        }
    }
    if (best.rgba.empty()) {
        if (error.empty()) {
            error = "Unable to decode an interior video frame";
        }
        return false;
    }
    output = std::move(best);
    return true;
}

} // namespace ps5mc
