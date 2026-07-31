#include "bfplayer/external_subtitles.hpp"

#include "bfplayer/diagnostics.hpp"
#include "bfplayer/safe_read_file.hpp"
#include "bfplayer/source_uri.hpp"

#include <SDL.h>

extern "C" {
#include <ass/ass.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavutil/rational.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bfplayer {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaximumSubtitleFileBytes =
    1024U * 1024U * 1024U;
constexpr std::size_t kMaximumCodecPrivateBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumTextBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumTextEvents = 100000;
constexpr std::size_t kMaximumSubtitlePackets = 200000;
constexpr unsigned int kMaximumRectsPerPacket = 4096;
constexpr int kMaximumBitmapDimension = 16384;

void ass_message(int level, const char* format, va_list arguments, void*) {
    if (level > 4) {
        return;
    }
    va_list copy;
    va_copy(copy, arguments);
    char message[2048]{};
    (void)std::vsnprintf(message, sizeof(message), format, copy);
    va_end(copy);
    diagnostics_log(
        level <= 2 ? DiagnosticLevel::error : DiagnosticLevel::warning,
        "libass level=%d message=%s",
        level,
        message);
    std::fputs("libass: ", stderr);
    std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
}

std::string ffmpeg_error(int value) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(value, buffer, sizeof(buffer));
    return buffer;
}

std::int64_t saturating_add(std::int64_t left, std::int64_t right) {
    if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return left + right;
}

std::int64_t saturating_subtract(std::int64_t left, std::int64_t right) {
    if (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right) {
        return std::numeric_limits<std::int64_t>::min();
    }
    if (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return left - right;
}

struct SubtitleInput {
    SafeReadFile file;
    AVFormatContext* format = nullptr;
    AVIOContext* custom_io = nullptr;
    Clock::time_point deadline{};

    ~SubtitleInput() {
        avformat_close_input(&format);
        if (custom_io) {
            av_freep(&custom_io->buffer);
            avio_context_free(&custom_io);
        }
    }

    [[nodiscard]] bool expired() const noexcept {
        return Clock::now() >= deadline;
    }

    static int interrupt(void* opaque) {
        const auto* input = static_cast<const SubtitleInput*>(opaque);
        return input && input->expired() ? 1 : 0;
    }

    static int read(void* opaque, std::uint8_t* buffer, int length) {
        auto* input = static_cast<SubtitleInput*>(opaque);
        if (!input || input->expired()) {
            return AVERROR_EXIT;
        }
        const int result = input->file.read(buffer, length);
        return result == 0 ? AVERROR_EOF : result;
    }

    static std::int64_t seek(void* opaque, std::int64_t offset, int whence) {
        auto* input = static_cast<SubtitleInput*>(opaque);
        if (!input || input->expired()) {
            return AVERROR_EXIT;
        }
        if ((whence & AVSEEK_SIZE) == AVSEEK_SIZE) {
            return input->file.size() >
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())
                ? AVERROR(EOVERFLOW)
                : static_cast<std::int64_t>(input->file.size());
        }
        const int origin = whence & ~AVSEEK_FORCE;
        if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) {
            return AVERROR(EINVAL);
        }
        return input->file.seek(offset, origin);
    }

    bool open(const std::string& path, std::string& error) {
        deadline = Clock::now() + std::chrono::seconds(30);
        format = avformat_alloc_context();
        if (!format) {
            error = "Unable to allocate subtitle format context";
            return false;
        }
        format->interrupt_callback.callback = interrupt;
        format->interrupt_callback.opaque = this;

        const bool network = is_network_uri(path);
        if (network && (!is_supported_stream_uri(path) ||
                        uri_has_credentials(path))) {
            error = "Unsupported or credential-bearing subtitle URL";
            return false;
        }
        if (!network) {
            if (!file.open(path, error)) {
                return false;
            }
            if (file.size() > kMaximumSubtitleFileBytes) {
                error = "Subtitle sidecar exceeds the 1 GiB input limit";
                return false;
            }
            constexpr int kBufferBytes = 64 * 1024;
            auto* buffer =
                static_cast<std::uint8_t*>(av_malloc(kBufferBytes));
            if (!buffer) {
                error = "Unable to allocate subtitle I/O buffer";
                return false;
            }
            custom_io = avio_alloc_context(
                buffer,
                kBufferBytes,
                0,
                this,
                read,
                nullptr,
                seek);
            if (!custom_io) {
                av_free(buffer);
                error = "Unable to allocate subtitle AVIO context";
                return false;
            }
            custom_io->seekable = AVIO_SEEKABLE_NORMAL;
            format->pb = custom_io;
            format->flags |= AVFMT_FLAG_CUSTOM_IO;
        }

        AVDictionary* options = nullptr;
        av_dict_set(&options, "probesize", "2097152", 0);
        av_dict_set(&options, "analyzeduration", "5000000", 0);
        av_dict_set(&options, "max_probe_packets", "10000", 0);
        if (network) {
            av_dict_set(&options, "rw_timeout", "10000000", 0);
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
        const int result =
            avformat_open_input(&format, path.c_str(), nullptr, &options);
        av_dict_free(&options);
        if (result < 0) {
            error = "avformat_open_input: " + ffmpeg_error(result);
            return false;
        }
        return true;
    }
};

bool is_bitmap_subtitle(AVCodecID codec) {
    switch (codec) {
        case AV_CODEC_ID_DVD_SUBTITLE:
        case AV_CODEC_ID_DVB_SUBTITLE:
        case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
        case AV_CODEC_ID_XSUB:
            return true;
        default:
            return false;
    }
}

std::int64_t packet_time_ms(const AVPacket& packet, const AVStream& stream) {
    std::int64_t timestamp = packet.pts;
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = packet.dts;
    }
    if (timestamp == AV_NOPTS_VALUE) {
        return 0;
    }
    return av_rescale_q(timestamp, stream.time_base, AVRational{1, 1000});
}

void blend_pixel(std::uint8_t* destination, std::uint8_t red, std::uint8_t green,
                 std::uint8_t blue, std::uint8_t alpha) {
    if (alpha == 0) {
        return;
    }
    const unsigned int destination_alpha = destination[3];
    const unsigned int inverse = 255U - alpha;
    const unsigned int output_alpha = alpha + (destination_alpha * inverse + 127U) / 255U;
    if (output_alpha == 0) {
        std::memset(destination, 0, 4);
        return;
    }
    const auto channel = [&](unsigned int source, unsigned int existing) {
        const unsigned int numerator =
            source * alpha * 255U + existing * destination_alpha * inverse;
        return static_cast<std::uint8_t>(
            std::min(255U, (numerator + output_alpha * 127U) / (output_alpha * 255U)));
    };
    destination[0] = channel(red, destination[0]);
    destination[1] = channel(green, destination[1]);
    destination[2] = channel(blue, destination[2]);
    destination[3] = static_cast<std::uint8_t>(output_alpha);
}

} // namespace

struct ExternalSubtitles::Impl {
    struct BitmapPart {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> pixels;
    };

    struct BitmapCue {
        std::int64_t start_ms = 0;
        std::int64_t end_ms = 0;
        int canvas_width = 0;
        int canvas_height = 0;
        std::vector<BitmapPart> parts;
    };

    SDL_Renderer* sdl_renderer = nullptr;
    SDL_Texture* texture = nullptr;
    ASS_Library* library = nullptr;
    ASS_Renderer* ass_renderer = nullptr;
    ASS_Track* track = nullptr;
    std::vector<std::uint8_t> rgba;
    std::vector<BitmapCue> bitmap_cues;
    std::string source_path;
    std::string last_error;
    int width = 0;
    int height = 0;
    bool populated = false;
    bool bitmap_mode = false;
    int last_bitmap_index = -2;
    SDL_Rect visible_rect{};
    bool has_visible_rect = false;

    void set_error(std::string value) { last_error = std::move(value); }

    void clear_visible_rect() {
        visible_rect = {};
        has_visible_rect = false;
    }

    void include_visible_rect(int x, int y, int rectangle_width, int rectangle_height) {
        const int left = std::clamp(x, 0, width);
        const int top = std::clamp(y, 0, height);
        const int right = std::clamp(x + rectangle_width, 0, width);
        const int bottom = std::clamp(y + rectangle_height, 0, height);
        if (right <= left || bottom <= top) {
            return;
        }
        if (!has_visible_rect) {
            visible_rect = {left, top, right - left, bottom - top};
            has_visible_rect = true;
            return;
        }
        const int union_left = std::min(visible_rect.x, left);
        const int union_top = std::min(visible_rect.y, top);
        const int union_right =
            std::max(visible_rect.x + visible_rect.w, right);
        const int union_bottom =
            std::max(visible_rect.y + visible_rect.h, bottom);
        visible_rect = {
            union_left,
            union_top,
            union_right - union_left,
            union_bottom - union_top};
    }

    bool draw_visible_texture() {
        if (!has_visible_rect) {
            return true;
        }
        if (SDL_RenderCopy(
                sdl_renderer,
                texture,
                &visible_rect,
                &visible_rect) != 0) {
            set_error(
                std::string("SDL_RenderCopy(subtitle): ") +
                SDL_GetError());
            return false;
        }
        return true;
    }

    void release() {
        SDL_DestroyTexture(texture);
        texture = nullptr;
        if (track) {
            ass_free_track(track);
            track = nullptr;
        }
        if (ass_renderer) {
            ass_renderer_done(ass_renderer);
            ass_renderer = nullptr;
        }
        if (library) {
            ass_library_done(library);
            library = nullptr;
        }
        rgba.clear();
        bitmap_cues.clear();
        source_path.clear();
        sdl_renderer = nullptr;
        width = 0;
        height = 0;
        populated = false;
        bitmap_mode = false;
        last_bitmap_index = -2;
        clear_visible_rect();
    }

    bool create_texture(int new_width, int new_height) {
        if (!sdl_renderer || new_width <= 0 || new_height <= 0 ||
            new_width > 7680 || new_height > 4320) {
            set_error("Invalid subtitle surface dimensions");
            return false;
        }
        if (static_cast<std::uint64_t>(new_width) * static_cast<std::uint64_t>(new_height) >
            std::numeric_limits<std::size_t>::max() / 4U) {
            set_error("Subtitle surface is too large");
            return false;
        }
        SDL_Texture* replacement = SDL_CreateTexture(
            sdl_renderer,
            SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING,
            new_width,
            new_height);
        if (!replacement) {
            set_error(std::string("SDL_CreateTexture: ") + SDL_GetError());
            return false;
        }
        SDL_SetTextureBlendMode(replacement, SDL_BLENDMODE_BLEND);
        SDL_DestroyTexture(texture);
        texture = replacement;
        width = new_width;
        height = new_height;
        rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0);
        if (ass_renderer) {
            ass_set_frame_size(ass_renderer, width, height);
            ass_set_storage_size(ass_renderer, width, height);
        }
        populated = false;
        last_bitmap_index = -2;
        clear_visible_rect();
        return true;
    }

    bool add_bitmap_cue(
        const AVSubtitle& subtitle,
        std::int64_t start_ms,
        std::int64_t duration_ms,
        int decoder_width,
        int decoder_height,
        std::size_t& total_bitmap_bytes) {
        constexpr std::size_t kMaximumBitmapBytes = 256U * 1024U * 1024U;
        constexpr std::size_t kMaximumBitmapCues = 20000;
        // PGS commonly marks a cue's end with a later empty display set while
        // end_display_time is UINT32_MAX. A new composition also replaces the
        // prior one, so cap the previous cue at this packet timestamp.
        if (!bitmap_cues.empty() && bitmap_cues.back().end_ms > start_ms) {
            bitmap_cues.back().end_ms = start_ms;
        }
        if (subtitle.num_rects == 0) {
            return true;
        }
        if (bitmap_cues.size() >= kMaximumBitmapCues) {
            set_error("External bitmap subtitle has too many cues");
            return false;
        }
        BitmapCue cue{};
        cue.start_ms = start_ms;
        cue.end_ms = saturating_add(start_ms, duration_ms);
        cue.canvas_width = decoder_width;
        cue.canvas_height = decoder_height;
        for (unsigned int index = 0; index < subtitle.num_rects; ++index) {
            const AVSubtitleRect* rectangle = subtitle.rects[index];
            if (!rectangle || rectangle->type != SUBTITLE_BITMAP) {
                continue;
            }
            if (rectangle->w <= 0 || rectangle->h <= 0 ||
                rectangle->w > kMaximumBitmapDimension ||
                rectangle->h > kMaximumBitmapDimension ||
                rectangle->x < 0 || rectangle->y < 0 ||
                rectangle->linesize[0] < rectangle->w ||
                !rectangle->data[0] || !rectangle->data[1] ||
                rectangle->nb_colors <= 0 || rectangle->nb_colors > 256) {
                set_error("External bitmap subtitle contains an invalid rectangle");
                return false;
            }
            const std::int64_t right =
                static_cast<std::int64_t>(rectangle->x) + rectangle->w;
            const std::int64_t bottom =
                static_cast<std::int64_t>(rectangle->y) + rectangle->h;
            if (right > kMaximumBitmapDimension ||
                bottom > kMaximumBitmapDimension) {
                set_error("External bitmap subtitle canvas exceeds limits");
                return false;
            }
            const std::uint64_t part_bytes64 =
                static_cast<std::uint64_t>(rectangle->w) *
                static_cast<std::uint64_t>(rectangle->h) * 4U;
            if (part_bytes64 > kMaximumBitmapBytes ||
                part_bytes64 > kMaximumBitmapBytes - total_bitmap_bytes) {
                set_error("External bitmap subtitle exceeds the 256 MiB cue cache");
                return false;
            }
            for (int row = 0; row < rectangle->h; ++row) {
                const std::uint8_t* indices = rectangle->data[0] +
                    static_cast<std::size_t>(row) *
                        static_cast<std::size_t>(rectangle->linesize[0]);
                for (int column = 0; column < rectangle->w; ++column) {
                    if (indices[column] >= rectangle->nb_colors) {
                        set_error(
                            "External bitmap subtitle references an invalid palette index");
                        return false;
                    }
                }
            }

            SDL_Surface* indexed = SDL_CreateRGBSurfaceWithFormatFrom(
                rectangle->data[0],
                rectangle->w,
                rectangle->h,
                8,
                rectangle->linesize[0],
                SDL_PIXELFORMAT_INDEX8);
            if (!indexed) {
                set_error(std::string("Bitmap subtitle indexed surface: ") + SDL_GetError());
                return false;
            }
            std::array<SDL_Color, 256> palette{};
            for (int color_index = 0;
                 color_index < rectangle->nb_colors;
                 ++color_index) {
                std::uint32_t color = 0;
                std::memcpy(
                    &color,
                    rectangle->data[1] +
                        static_cast<std::size_t>(color_index) * sizeof(color),
                    sizeof(color));
                palette[static_cast<std::size_t>(color_index)] = SDL_Color{
                    static_cast<std::uint8_t>((color >> 16U) & 0xffU),
                    static_cast<std::uint8_t>((color >> 8U) & 0xffU),
                    static_cast<std::uint8_t>(color & 0xffU),
                    static_cast<std::uint8_t>((color >> 24U) & 0xffU)};
            }
            SDL_Surface* converted = SDL_CreateRGBSurfaceWithFormat(
                0, rectangle->w, rectangle->h, 32, SDL_PIXELFORMAT_RGBA32);
            if (!converted ||
                SDL_SetPaletteColors(
                    indexed->format->palette,
                    palette.data(),
                    0,
                    rectangle->nb_colors) != 0 ||
                SDL_BlitSurface(indexed, nullptr, converted, nullptr) != 0) {
                SDL_FreeSurface(indexed);
                SDL_FreeSurface(converted);
                set_error(std::string("Bitmap subtitle conversion: ") + SDL_GetError());
                return false;
            }

            const std::size_t part_bytes =
                static_cast<std::size_t>(part_bytes64);
            BitmapPart part{};
            part.x = rectangle->x;
            part.y = rectangle->y;
            part.width = rectangle->w;
            part.height = rectangle->h;
            part.pixels.resize(part_bytes);
            for (int row = 0; row < part.height; ++row) {
                std::memcpy(
                    part.pixels.data() + static_cast<std::size_t>(row) *
                        static_cast<std::size_t>(part.width) * 4U,
                    static_cast<const std::uint8_t*>(converted->pixels) +
                        static_cast<std::size_t>(row) * static_cast<std::size_t>(converted->pitch),
                    static_cast<std::size_t>(part.width) * 4U);
            }
            total_bitmap_bytes += part_bytes;
            cue.canvas_width = std::max(cue.canvas_width, static_cast<int>(right));
            cue.canvas_height = std::max(cue.canvas_height, static_cast<int>(bottom));
            cue.parts.push_back(std::move(part));
            SDL_FreeSurface(indexed);
            SDL_FreeSurface(converted);
        }
        if (!cue.parts.empty()) {
            cue.canvas_width = std::max(1, cue.canvas_width);
            cue.canvas_height = std::max(1, cue.canvas_height);
            bitmap_cues.push_back(std::move(cue));
            return true;
        }
        return false;
    }

    bool decode_subtitle_file(const std::string& path) {
        SubtitleInput input;
        std::string input_error;
        if (!input.open(path, input_error)) {
            set_error(std::move(input_error));
            return false;
        }
        AVFormatContext* format = input.format;
        AVCodecContext* codec_context = nullptr;
        const AVCodec* codec = nullptr;
        int result = avformat_find_stream_info(format, nullptr);
        if (result < 0) {
            set_error("avformat_find_stream_info: " + ffmpeg_error(result));
            return false;
        }
        const int stream_index = av_find_best_stream(
            format, AVMEDIA_TYPE_SUBTITLE, -1, -1, &codec, 0);
        if (stream_index < 0 || !codec) {
            set_error("No decodable subtitle stream in sidecar");
            return false;
        }
        AVStream* stream = format->streams[stream_index];
        bitmap_mode = is_bitmap_subtitle(stream->codecpar->codec_id);

        codec_context = avcodec_alloc_context3(codec);
        if (!codec_context) {
            set_error("Unable to allocate subtitle decoder");
            return false;
        }
        result = avcodec_parameters_to_context(codec_context, stream->codecpar);
        if (result >= 0) {
            result = avcodec_open2(codec_context, codec, nullptr);
        }
        if (result < 0) {
            set_error("Unable to open subtitle decoder: " + ffmpeg_error(result));
            avcodec_free_context(&codec_context);
            return false;
        }

        if (codec_context->subtitle_header && codec_context->subtitle_header_size > 0) {
            if (static_cast<std::size_t>(codec_context->subtitle_header_size) >
                kMaximumCodecPrivateBytes) {
                set_error("Subtitle codec-private data exceeds the 4 MiB limit");
                avcodec_free_context(&codec_context);
                return false;
            }
            ass_process_codec_private(
                track,
                reinterpret_cast<const char*>(codec_context->subtitle_header),
                codec_context->subtitle_header_size);
        }
        ass_set_check_readorder(track, 0);

        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            set_error("Unable to allocate subtitle packet");
            avcodec_free_context(&codec_context);
            return false;
        }

        bool any_events = false;
        std::size_t total_bitmap_bytes = 0;
        std::size_t total_text_bytes = 0;
        std::size_t total_text_events = 0;
        std::size_t total_packets = 0;
        while ((result = av_read_frame(format, packet)) >= 0) {
            if (++total_packets > kMaximumSubtitlePackets || input.expired()) {
                av_packet_unref(packet);
                set_error(input.expired()
                    ? "External subtitle decode exceeded the 30 second deadline"
                    : "External subtitle has too many packets");
                av_packet_free(&packet);
                avcodec_free_context(&codec_context);
                return false;
            }
            if (packet->stream_index == stream_index) {
                AVSubtitle subtitle{};
                int got_subtitle = 0;
                const int decoded = avcodec_decode_subtitle2(
                    codec_context, &subtitle, &got_subtitle, packet);
                if (decoded < 0) {
                    av_packet_unref(packet);
                    set_error("Subtitle decode failed: " + ffmpeg_error(decoded));
                    av_packet_free(&packet);
                    avcodec_free_context(&codec_context);
                    return false;
                }
                if (got_subtitle) {
                    if (subtitle.num_rects > kMaximumRectsPerPacket) {
                        avsubtitle_free(&subtitle);
                        av_packet_unref(packet);
                        set_error("External subtitle packet has too many rectangles");
                        av_packet_free(&packet);
                        avcodec_free_context(&codec_context);
                        return false;
                    }
                    const std::int64_t packet_ms = packet_time_ms(*packet, *stream);
                    const std::int64_t start_ms = saturating_add(
                        packet_ms,
                        static_cast<std::int64_t>(subtitle.start_display_time));
                    const std::int64_t end_ms = saturating_add(
                        packet_ms,
                        static_cast<std::int64_t>(subtitle.end_display_time));
                    std::int64_t duration_ms =
                        saturating_subtract(end_ms, start_ms);
                    if (duration_ms <= 0 && packet->duration > 0) {
                        duration_ms = av_rescale_q(
                            packet->duration, stream->time_base, AVRational{1, 1000});
                    }
                    duration_ms = std::max<std::int64_t>(1, duration_ms);
                    if (bitmap_mode) {
                        if (!add_bitmap_cue(
                                subtitle,
                                start_ms,
                                duration_ms,
                                codec_context->width,
                                codec_context->height,
                                total_bitmap_bytes) && !last_error.empty()) {
                            avsubtitle_free(&subtitle);
                            av_packet_unref(packet);
                            av_packet_free(&packet);
                            avcodec_free_context(&codec_context);
                            return false;
                        }
                        any_events = any_events || !bitmap_cues.empty();
                    } else {
                        for (unsigned int index = 0; index < subtitle.num_rects; ++index) {
                            const AVSubtitleRect* rectangle = subtitle.rects[index];
                            if (rectangle && rectangle->ass && rectangle->ass[0]) {
                                const std::size_t size = std::strlen(rectangle->ass);
                                if (size >
                                        static_cast<std::size_t>(
                                            std::numeric_limits<int>::max()) ||
                                    total_text_events >= kMaximumTextEvents ||
                                    size > kMaximumTextBytes - total_text_bytes) {
                                    avsubtitle_free(&subtitle);
                                    av_packet_unref(packet);
                                    set_error(
                                        "External text subtitle exceeds event/byte limits");
                                    av_packet_free(&packet);
                                    avcodec_free_context(&codec_context);
                                    return false;
                                }
                                ass_process_chunk(
                                    track,
                                    rectangle->ass,
                                    static_cast<int>(size),
                                    start_ms,
                                    duration_ms);
                                total_text_bytes += size;
                                ++total_text_events;
                                any_events = true;
                            }
                        }
                    }
                    avsubtitle_free(&subtitle);
                }
            }
            av_packet_unref(packet);
        }

        av_packet_free(&packet);
        avcodec_free_context(&codec_context);
        if (result != AVERROR_EOF) {
            set_error("Subtitle read failed: " + ffmpeg_error(result));
            return false;
        }
        if (!any_events) {
            set_error(bitmap_mode
                ? "Subtitle sidecar contained no bitmap cues"
                : "Subtitle sidecar contained no text events");
            return false;
        }
        return true;
    }

    bool draw_bitmap(std::int64_t position_ms) {
        int active = -1;
        const auto after = std::upper_bound(
            bitmap_cues.begin(),
            bitmap_cues.end(),
            position_ms,
            [](std::int64_t value, const BitmapCue& cue) {
                return value < cue.start_ms;
            });
        if (after != bitmap_cues.begin()) {
            const auto candidate = std::prev(after);
            if (position_ms < candidate->end_ms) {
                active = static_cast<int>(std::distance(bitmap_cues.begin(), candidate));
            }
        }
        if (active != last_bitmap_index) {
            std::fill(rgba.begin(), rgba.end(), 0);
            clear_visible_rect();
            if (active >= 0) {
                const BitmapCue& cue = bitmap_cues[static_cast<std::size_t>(active)];
                const double scale_x = static_cast<double>(width) / cue.canvas_width;
                const double scale_y = static_cast<double>(height) / cue.canvas_height;
                for (const BitmapPart& part : cue.parts) {
                    const int target_x = static_cast<int>(std::llround(part.x * scale_x));
                    const int target_y = static_cast<int>(std::llround(part.y * scale_y));
                    const int target_width = std::max(1, static_cast<int>(std::llround(part.width * scale_x)));
                    const int target_height = std::max(1, static_cast<int>(std::llround(part.height * scale_y)));
                    include_visible_rect(
                        target_x,
                        target_y,
                        target_width,
                        target_height);
                    for (int y = 0; y < target_height; ++y) {
                        const int destination_y = target_y + y;
                        if (destination_y < 0 || destination_y >= height) {
                            continue;
                        }
                        const int source_y = std::min(
                            part.height - 1,
                            static_cast<int>(static_cast<double>(y) / scale_y));
                        for (int x = 0; x < target_width; ++x) {
                            const int destination_x = target_x + x;
                            if (destination_x < 0 || destination_x >= width) {
                                continue;
                            }
                            const int source_x = std::min(
                                part.width - 1,
                                static_cast<int>(static_cast<double>(x) / scale_x));
                            const std::uint8_t* source = part.pixels.data() +
                                (static_cast<std::size_t>(source_y) *
                                     static_cast<std::size_t>(part.width) +
                                 static_cast<std::size_t>(source_x)) * 4U;
                            std::uint8_t* destination = rgba.data() +
                                (static_cast<std::size_t>(destination_y) *
                                     static_cast<std::size_t>(width) +
                                 static_cast<std::size_t>(destination_x)) * 4U;
                            blend_pixel(
                                destination, source[0], source[1], source[2], source[3]);
                        }
                    }
                }
            }
            if (SDL_UpdateTexture(texture, nullptr, rgba.data(), width * 4) != 0) {
                set_error(std::string("SDL_UpdateTexture(bitmap): ") + SDL_GetError());
                return false;
            }
            last_bitmap_index = active;
            populated = true;
        }
        return draw_visible_texture();
    }

    bool draw(std::int64_t position_ms) {
        if (bitmap_mode) {
            return draw_bitmap(position_ms);
        }
        int changed = 0;
        ASS_Image* images = ass_render_frame(ass_renderer, track, position_ms, &changed);
        if (!changed && populated) {
            return draw_visible_texture();
        }

        std::fill(rgba.begin(), rgba.end(), 0);
        clear_visible_rect();
        for (const ASS_Image* image = images; image; image = image->next) {
            include_visible_rect(
                image->dst_x,
                image->dst_y,
                image->w,
                image->h);
            const std::uint8_t red = static_cast<std::uint8_t>((image->color >> 24U) & 0xffU);
            const std::uint8_t green = static_cast<std::uint8_t>((image->color >> 16U) & 0xffU);
            const std::uint8_t blue = static_cast<std::uint8_t>((image->color >> 8U) & 0xffU);
            const unsigned int color_opacity = 255U - (image->color & 0xffU);
            for (int row = 0; row < image->h; ++row) {
                const int y = image->dst_y + row;
                if (y < 0 || y >= height) {
                    continue;
                }
                for (int column = 0; column < image->w; ++column) {
                    const int x = image->dst_x + column;
                    if (x < 0 || x >= width) {
                        continue;
                    }
                    const unsigned int mask = image->bitmap[row * image->stride + column];
                    const std::uint8_t alpha = static_cast<std::uint8_t>(
                        (mask * color_opacity + 127U) / 255U);
                    std::uint8_t* pixel = rgba.data() +
                        (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                         static_cast<std::size_t>(x)) * 4U;
                    blend_pixel(pixel, red, green, blue, alpha);
                }
            }
        }
        if (SDL_UpdateTexture(texture, nullptr, rgba.data(), width * 4) != 0) {
            set_error(std::string("SDL_UpdateTexture: ") + SDL_GetError());
            return false;
        }
        populated = true;
        return draw_visible_texture();
    }
};

ExternalSubtitles::ExternalSubtitles() : impl_(std::make_unique<Impl>()) {}

ExternalSubtitles::~ExternalSubtitles() {
    close();
}

bool ExternalSubtitles::open(
    SDL_Renderer* renderer,
    const std::string& path,
    const std::string& fallback_font,
    int screen_width,
    int screen_height) {
    close();
    impl_->last_error.clear();
    diagnostics_log(
        DiagnosticLevel::info,
        "external-subtitle decode begin path=%s screen=%dx%d",
        redact_uri_secrets(path).c_str(),
        screen_width,
        screen_height);
    if (!renderer || path.empty() || fallback_font.empty()) {
        impl_->set_error("ExternalSubtitles::open: invalid argument");
        diagnostics_log(DiagnosticLevel::error, "external-subtitle decode rejected reason=invalid-argument");
        return false;
    }
    impl_->sdl_renderer = renderer;
    impl_->library = ass_library_init();
    if (!impl_->library) {
        impl_->set_error("ass_library_init failed");
        diagnostics_log(DiagnosticLevel::error, "external-subtitle decode failed error=%s", impl_->last_error.c_str());
        impl_->release();
        return false;
    }
    ass_set_message_cb(impl_->library, ass_message, nullptr);
    impl_->ass_renderer = ass_renderer_init(impl_->library);
    impl_->track = ass_new_track(impl_->library);
    if (!impl_->ass_renderer || !impl_->track) {
        impl_->set_error("Unable to initialize libass renderer");
        const std::string error = impl_->last_error;
        impl_->release();
        impl_->last_error = error;
        diagnostics_log(DiagnosticLevel::error, "external-subtitle decode failed error=%s", impl_->last_error.c_str());
        return false;
    }
    ass_set_fonts(
        impl_->ass_renderer,
        fallback_font.c_str(),
        "Noto Sans",
        ASS_FONTPROVIDER_NONE,
        nullptr,
        0);
    if (!impl_->create_texture(screen_width, screen_height) ||
        !impl_->decode_subtitle_file(path)) {
        const std::string error = impl_->last_error;
        impl_->release();
        impl_->last_error = error;
        diagnostics_log(
            DiagnosticLevel::error,
            "external-subtitle decode failed path=%s error=%s",
            redact_uri_secrets(path).c_str(),
            impl_->last_error.c_str());
        return false;
    }
    impl_->source_path = path;
    diagnostics_log(
        DiagnosticLevel::info,
        "external-subtitle decode success path=%s bitmap_cues=%zu bitmap=%d",
        redact_uri_secrets(path).c_str(),
        impl_->bitmap_cues.size(),
        impl_->bitmap_mode ? 1 : 0);
    return true;
}

void ExternalSubtitles::close() {
    if (impl_) {
        const std::string old_error = impl_->last_error;
        impl_->release();
        impl_->last_error = old_error;
    }
}

bool ExternalSubtitles::resize(int screen_width, int screen_height) {
    return impl_ && impl_->track && impl_->create_texture(screen_width, screen_height);
}

bool ExternalSubtitles::render(std::int64_t movie_position_ms, std::int64_t delay_ms) {
    if (!is_open()) {
        return false;
    }
    // Positive delay means "show subtitles later", so render an earlier
    // subtitle clock relative to the movie clock.
    std::int64_t target = movie_position_ms;
    if (delay_ms > 0 && target < delay_ms) {
        target = 0;
    } else if (delay_ms < 0 && target > std::numeric_limits<std::int64_t>::max() + delay_ms) {
        target = std::numeric_limits<std::int64_t>::max();
    } else {
        target -= delay_ms;
    }
    return impl_->draw(std::max<std::int64_t>(0, target));
}

bool ExternalSubtitles::visible_bounds(
    int& x,
    int& y,
    int& width,
    int& height) const noexcept {
    if (!impl_ || !impl_->has_visible_rect) {
        x = 0;
        y = 0;
        width = 0;
        height = 0;
        return false;
    }
    x = impl_->visible_rect.x;
    y = impl_->visible_rect.y;
    width = impl_->visible_rect.w;
    height = impl_->visible_rect.h;
    return true;
}

bool ExternalSubtitles::is_open() const noexcept {
    return impl_ && impl_->track && impl_->texture && !impl_->source_path.empty();
}

const std::string& ExternalSubtitles::path() const noexcept {
    return impl_->source_path;
}

const std::string& ExternalSubtitles::error() const noexcept {
    return impl_->last_error;
}

} // namespace bfplayer
