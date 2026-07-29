#include "bfplayer/hdr_tonemap.hpp"
#include "bfplayer/hdr_yuv_tonemap.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unistd.h>

#ifdef _WIN32
#include <io.h>
#endif

#if defined(BFPLAYER_PS5)
#include <pthread.h>
#endif

namespace bfplayer {
namespace {

constexpr double kDefaultSourcePeakNits = 1000.0;
constexpr double kDefaultTargetPeakNits = 100.0;
constexpr int kMinimumCubeSize = 2;
constexpr int kMaximumCubeSize = 65;

double clamp_unit(double value) {
    if (!std::isfinite(value)) {
        return 0.0;
    }
    return std::clamp(value, 0.0, 1.0);
}

double pq_eotf_nits(double encoded) {
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 32.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 128.0;
    constexpr double c3 = 2392.0 / 128.0;

    const double value = clamp_unit(encoded);
    const double power = std::pow(value, 1.0 / m2);
    const double numerator = std::max(power - c1, 0.0);
    const double denominator = std::max(c2 - c3 * power, 1.0e-12);
    return 10000.0 * std::pow(numerator / denominator, 1.0 / m1);
}

double hlg_inverse_oetf(double encoded) {
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;

    const double value = clamp_unit(encoded);
    if (value <= 0.5) {
        return value * value / 3.0;
    }
    return (std::exp((value - c) / a) + b) / 12.0;
}

double hable_curve(double value) {
    constexpr double a = 0.15;
    constexpr double b = 0.50;
    constexpr double c = 0.10;
    constexpr double d = 0.20;
    constexpr double e = 0.02;
    constexpr double f = 0.30;

    const double x = std::max(value, 0.0);
    return ((x * (x * a + b * c) + d * e) /
            (x * (x * a + b) + d * f)) -
        e / f;
}

RgbTriplet decode_to_linear_bt2020(
    RgbTriplet encoded,
    const HdrToneMapSettings& settings) {
    if (settings.transfer == HdrTransfer::pq) {
        const double target_peak = settings.target_peak_nits > 0.0
            ? settings.target_peak_nits
            : kDefaultTargetPeakNits;
        return {
            pq_eotf_nits(encoded.red) / target_peak,
            pq_eotf_nits(encoded.green) / target_peak,
            pq_eotf_nits(encoded.blue) / target_peak,
        };
    }

    RgbTriplet scene{
        hlg_inverse_oetf(encoded.red),
        hlg_inverse_oetf(encoded.green),
        hlg_inverse_oetf(encoded.blue),
    };
    const double scene_luma = std::max(
        0.2627 * scene.red +
            0.6780 * scene.green +
            0.0593 * scene.blue,
        0.0);
    const double source_peak = settings.source_peak_nits > 0.0
        ? settings.source_peak_nits
        : kDefaultSourcePeakNits;
    const double target_peak = settings.target_peak_nits > 0.0
        ? settings.target_peak_nits
        : kDefaultTargetPeakNits;
    const double system_gamma = std::clamp(
        1.2 + 0.42 * std::log10(source_peak / 1000.0),
        1.0,
        1.5);
    const double ootf = (source_peak / target_peak) *
        std::pow(std::max(scene_luma, 1.0e-12), system_gamma - 1.0);
    scene.red *= ootf;
    scene.green *= ootf;
    scene.blue *= ootf;
    return scene;
}

RgbTriplet tone_map_linear_bt2020(
    RgbTriplet linear,
    const HdrToneMapSettings& settings) {
    const double source_peak = settings.source_peak_nits > 0.0
        ? settings.source_peak_nits
        : kDefaultSourcePeakNits;
    const double target_peak = settings.target_peak_nits > 0.0
        ? settings.target_peak_nits
        : kDefaultTargetPeakNits;
    const double peak = std::max(source_peak / target_peak, 1.0);

    const double luma = std::max(
        0.2627 * linear.red +
            0.6780 * linear.green +
            0.0593 * linear.blue,
        0.0);
    constexpr double desaturation_threshold = 2.0;
    if (luma > desaturation_threshold) {
        const double blend = std::clamp(
            (luma - desaturation_threshold) / std::max(luma, 1.0e-12),
            0.0,
            1.0);
        linear.red += (luma - linear.red) * blend;
        linear.green += (luma - linear.green) * blend;
        linear.blue += (luma - linear.blue) * blend;
    }

    const double signal = std::max({
        linear.red,
        linear.green,
        linear.blue,
        0.0,
    });
    if (signal <= 1.0e-12) {
        return {};
    }
    const double white = std::max(hable_curve(peak), 1.0e-12);
    const double mapped = hable_curve(signal) / white;
    const double scale = mapped / signal;
    linear.red *= scale;
    linear.green *= scale;
    linear.blue *= scale;
    return linear;
}

RgbTriplet bt2020_to_bt709(RgbTriplet value) {
    return {
        1.660491 * value.red -
            0.587641 * value.green -
            0.072850 * value.blue,
        -0.124550 * value.red +
            1.132900 * value.green -
            0.008350 * value.blue,
        -0.018151 * value.red -
            0.100579 * value.green +
            1.118730 * value.blue,
    };
}

RgbTriplet compress_bt709_gamut(RgbTriplet value);
double bt709_oetf(double linear);

RgbTriplet tone_map_hdr_to_bt709_sdr(
    RgbTriplet encoded,
    const HdrToneMapSettings& settings,
    bool input_bt2020) {
    RgbTriplet linear = decode_to_linear_bt2020(encoded, settings);
    linear = tone_map_linear_bt2020(linear, settings);
    if (input_bt2020) {
        linear = bt2020_to_bt709(linear);
    }
    linear = compress_bt709_gamut(linear);
    return {
        bt709_oetf(linear.red),
        bt709_oetf(linear.green),
        bt709_oetf(linear.blue),
    };
}

RgbTriplet decode_yuv(
    double y,
    double u,
    double v,
    bool bt2020) {
    const double kr = bt2020 ? 0.2627 : 0.2126;
    const double kb = bt2020 ? 0.0593 : 0.0722;
    const double kg = 1.0 - kr - kb;
    return {
        y + 2.0 * (1.0 - kr) * v,
        y -
            2.0 * kb * (1.0 - kb) / kg * u -
            2.0 * kr * (1.0 - kr) / kg * v,
        y + 2.0 * (1.0 - kb) * u,
    };
}

RgbTriplet encode_bt709_yuv(RgbTriplet encoded_rgb) {
    constexpr double kr = 0.2126;
    constexpr double kb = 0.0722;
    constexpr double kg = 1.0 - kr - kb;
    const double y =
        kr * encoded_rgb.red +
        kg * encoded_rgb.green +
        kb * encoded_rgb.blue;
    return {
        y,
        (encoded_rgb.blue - y) / (2.0 * (1.0 - kb)),
        (encoded_rgb.red - y) / (2.0 * (1.0 - kr)),
    };
}

double decode_luma_code(int code, bool full_range) {
    if (full_range) {
        return clamp_unit(static_cast<double>(code) / 255.0);
    }
    return clamp_unit((static_cast<double>(code) - 16.0) / 219.0);
}

double decode_chroma_code(int code, bool full_range) {
    const double denominator = full_range ? 255.0 : 224.0;
    return std::clamp(
        (static_cast<double>(code) - 128.0) / denominator,
        -0.5,
        0.5);
}

std::uint8_t encode_limited_luma(double value) {
    const long code = std::lround(16.0 + 219.0 * clamp_unit(value));
    return static_cast<std::uint8_t>(std::clamp(code, 16L, 235L));
}

std::uint8_t encode_limited_chroma(double value) {
    const long code = std::lround(
        128.0 + 224.0 * std::clamp(value, -0.5, 0.5));
    return static_cast<std::uint8_t>(std::clamp(code, 16L, 240L));
}

std::size_t yuv_lut_index(int y, int u_level, int v_level) {
    return static_cast<std::size_t>(
        ((u_level * BFPLAYER_HDR_CHROMA_LEVELS + v_level) *
             BFPLAYER_HDR_LUMA_LEVELS) +
        y);
}

int quantize_chroma(std::uint8_t value) {
    return (
        static_cast<int>(value) *
            (BFPLAYER_HDR_CHROMA_LEVELS - 1) +
        127) /
        255;
}

std::uint8_t packed_y(const BfplayerHdrYuvEntry& entry) {
    return static_cast<std::uint8_t>(entry.packed);
}

std::uint8_t packed_u(const BfplayerHdrYuvEntry& entry) {
    return static_cast<std::uint8_t>(entry.packed >> 8U);
}

std::uint8_t packed_v(const BfplayerHdrYuvEntry& entry) {
    return static_cast<std::uint8_t>(entry.packed >> 16U);
}

void apply_hdr_yuv420_rows(
    const BfplayerHdrYuvLut* lut,
    std::uint8_t* y_plane,
    int y_stride,
    std::uint8_t* u_plane,
    int u_stride,
    std::uint8_t* v_plane,
    int v_stride,
    int width,
    int height,
    int chroma_y_begin,
    int chroma_y_end) {
    const int paired_width = width & ~1;
    for (int chroma_y = chroma_y_begin;
         chroma_y < chroma_y_end;
         ++chroma_y) {
        std::uint8_t* const u_row =
            u_plane + static_cast<std::size_t>(chroma_y) * u_stride;
        std::uint8_t* const v_row =
            v_plane + static_cast<std::size_t>(chroma_y) * v_stride;
        const int top_y = chroma_y * 2;
        std::uint8_t* const y_top =
            y_plane + static_cast<std::size_t>(top_y) * y_stride;
        std::uint8_t* const y_bottom =
            top_y + 1 < height
            ? y_top + y_stride
            : nullptr;

        int pixel_x = 0;
        for (; pixel_x < paired_width; pixel_x += 2) {
            const int chroma_x = pixel_x / 2;
            const int u_level = quantize_chroma(u_row[chroma_x]);
            const int v_level = quantize_chroma(v_row[chroma_x]);
            const BfplayerHdrYuvEntry* const entries =
                lut->entries +
                yuv_lut_index(0, u_level, v_level);
            const BfplayerHdrYuvEntry first = entries[y_top[pixel_x]];
            const BfplayerHdrYuvEntry second =
                entries[y_top[pixel_x + 1]];
            y_top[pixel_x] = packed_y(first);
            y_top[pixel_x + 1] = packed_y(second);
            unsigned int u_sum = packed_u(first) + packed_u(second);
            unsigned int v_sum = packed_v(first) + packed_v(second);
            unsigned int count = 2;
            if (y_bottom) {
                const BfplayerHdrYuvEntry third =
                    entries[y_bottom[pixel_x]];
                const BfplayerHdrYuvEntry fourth =
                    entries[y_bottom[pixel_x + 1]];
                y_bottom[pixel_x] = packed_y(third);
                y_bottom[pixel_x + 1] = packed_y(fourth);
                u_sum += packed_u(third) + packed_u(fourth);
                v_sum += packed_v(third) + packed_v(fourth);
                count = 4;
            }
            u_row[chroma_x] = static_cast<std::uint8_t>(
                (u_sum + count / 2) / count);
            v_row[chroma_x] = static_cast<std::uint8_t>(
                (v_sum + count / 2) / count);
        }
        if (pixel_x < width) {
            const int chroma_x = pixel_x / 2;
            const int u_level = quantize_chroma(u_row[chroma_x]);
            const int v_level = quantize_chroma(v_row[chroma_x]);
            const BfplayerHdrYuvEntry* const entries =
                lut->entries +
                yuv_lut_index(0, u_level, v_level);
            const BfplayerHdrYuvEntry first = entries[y_top[pixel_x]];
            y_top[pixel_x] = packed_y(first);
            unsigned int u_sum = packed_u(first);
            unsigned int v_sum = packed_v(first);
            unsigned int count = 1;
            if (y_bottom) {
                const BfplayerHdrYuvEntry second =
                    entries[y_bottom[pixel_x]];
                y_bottom[pixel_x] = packed_y(second);
                u_sum += packed_u(second);
                v_sum += packed_v(second);
                count = 2;
            }
            u_row[chroma_x] = static_cast<std::uint8_t>(
                (u_sum + count / 2) / count);
            v_row[chroma_x] = static_cast<std::uint8_t>(
                (v_sum + count / 2) / count);
        }
    }
}

RgbTriplet compress_bt709_gamut(RgbTriplet value) {
    const double luma = std::clamp(
        0.2126 * value.red +
            0.7152 * value.green +
            0.0722 * value.blue,
        0.0,
        1.0);
    const double minimum = std::min({
        value.red,
        value.green,
        value.blue,
    });
    const double maximum = std::max({
        value.red,
        value.green,
        value.blue,
    });
    double saturation_scale = 1.0;
    if (minimum < 0.0 && luma > minimum) {
        saturation_scale = std::min(
            saturation_scale,
            luma / (luma - minimum));
    }
    if (maximum > 1.0 && maximum > luma) {
        saturation_scale = std::min(
            saturation_scale,
            (1.0 - luma) / (maximum - luma));
    }
    value.red = luma + (value.red - luma) * saturation_scale;
    value.green = luma + (value.green - luma) * saturation_scale;
    value.blue = luma + (value.blue - luma) * saturation_scale;
    value.red = clamp_unit(value.red);
    value.green = clamp_unit(value.green);
    value.blue = clamp_unit(value.blue);
    return value;
}

double bt709_oetf(double linear) {
    const double value = clamp_unit(linear);
    if (value < 0.018) {
        return 4.5 * value;
    }
    return 1.099 * std::pow(value, 0.45) - 0.099;
}

bool valid_settings(const HdrToneMapSettings& settings, std::string& error) {
    if (!std::isfinite(settings.source_peak_nits) ||
        settings.source_peak_nits <= 0.0) {
        error = "HDR source peak must be positive";
        return false;
    }
    if (!std::isfinite(settings.target_peak_nits) ||
        settings.target_peak_nits <= 0.0) {
        error = "SDR target peak must be positive";
        return false;
    }
    if (settings.source_peak_nits < settings.target_peak_nits) {
        error = "HDR source peak must not be below the SDR target peak";
        return false;
    }
    if (settings.cube_size < kMinimumCubeSize ||
        settings.cube_size > kMaximumCubeSize) {
        error = "HDR LUT cube size must be between 2 and 65";
        return false;
    }
    return true;
}

bool write_all(int descriptor, const char* bytes, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(
            descriptor,
            bytes + offset,
            size - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            errno = EIO;
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

int sync_descriptor(int descriptor) {
#ifdef _WIN32
    return ::_commit(descriptor);
#else
    return ::fsync(descriptor);
#endif
}

} // namespace

RgbTriplet tone_map_bt2020_hdr_to_bt709_sdr(
    RgbTriplet encoded_bt2020,
    const HdrToneMapSettings& settings) {
    return tone_map_hdr_to_bt709_sdr(
        encoded_bt2020,
        settings,
        true);
}

bool make_hdr_tonemap_cube(
    const HdrToneMapSettings& settings,
    std::string& cube,
    std::string& error) {
    cube.clear();
    error.clear();
    if (!valid_settings(settings, error)) {
        return false;
    }

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "TITLE \"BFplayer "
           << (settings.transfer == HdrTransfer::pq ? "PQ" : "HLG")
           << " BT.2020 HDR to BT.709 SDR\"\n"
           << "LUT_3D_SIZE " << settings.cube_size << '\n'
           << "DOMAIN_MIN 0.0 0.0 0.0\n"
           << "DOMAIN_MAX 1.0 1.0 1.0\n";
    output << std::fixed << std::setprecision(9);

    const double denominator =
        static_cast<double>(settings.cube_size - 1);
    for (int blue = 0; blue < settings.cube_size; ++blue) {
        for (int green = 0; green < settings.cube_size; ++green) {
            for (int red = 0; red < settings.cube_size; ++red) {
                const RgbTriplet mapped =
                    tone_map_bt2020_hdr_to_bt709_sdr(
                        {
                            red / denominator,
                            green / denominator,
                            blue / denominator,
                        },
                        settings);
                output << mapped.red << ' '
                       << mapped.green << ' '
                       << mapped.blue << '\n';
            }
        }
    }
    cube = output.str();
    if (cube.empty()) {
        error = "HDR LUT generation produced no data";
        return false;
    }
    return true;
}

bool write_hdr_tonemap_cube_atomic(
    const std::string& path,
    const HdrToneMapSettings& settings,
    std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "HDR LUT path is empty";
        return false;
    }
    std::string cube;
    if (!make_hdr_tonemap_cube(settings, cube, error)) {
        return false;
    }
    const std::string temporary = path + ".tmp";
    const int descriptor = ::open(
        temporary.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC,
        0644);
    if (descriptor < 0) {
        error = "Open HDR LUT temporary file: ";
        error += std::strerror(errno);
        return false;
    }

    bool success = write_all(descriptor, cube.data(), cube.size());
    int saved_error = success ? 0 : errno;
    if (success && sync_descriptor(descriptor) != 0) {
        success = false;
        saved_error = errno;
    }
    if (::close(descriptor) != 0 && success) {
        success = false;
        saved_error = errno;
    }
    if (success && ::rename(temporary.c_str(), path.c_str()) != 0) {
        success = false;
        saved_error = errno;
    }
    if (!success) {
        (void)::unlink(temporary.c_str());
        error = "Write HDR LUT: ";
        error += std::strerror(saved_error ? saved_error : EIO);
        return false;
    }
    return true;
}

} // namespace bfplayer

extern "C" int bfplayer_build_hdr_yuv420_lut(
    const BfplayerHdrYuvConfig* config,
    BfplayerHdrYuvLut* lut) {
    if (!config || !lut ||
        (config->transfer != BFPLAYER_HDR_TRANSFER_PQ &&
         config->transfer != BFPLAYER_HDR_TRANSFER_HLG) ||
        !std::isfinite(config->source_peak_nits) ||
        config->source_peak_nits <= 0.0 ||
        !std::isfinite(config->target_peak_nits) ||
        config->target_peak_nits <= 0.0 ||
        config->source_peak_nits < config->target_peak_nits) {
        return -1;
    }

    bfplayer::HdrToneMapSettings settings;
    settings.transfer =
        config->transfer == BFPLAYER_HDR_TRANSFER_HLG
        ? bfplayer::HdrTransfer::hlg
        : bfplayer::HdrTransfer::pq;
    settings.source_peak_nits = config->source_peak_nits;
    settings.target_peak_nits = config->target_peak_nits;
    lut->config = *config;

    for (int u_level = 0;
         u_level < BFPLAYER_HDR_CHROMA_LEVELS;
         ++u_level) {
        const int u_code = static_cast<int>(std::lround(
            static_cast<double>(u_level) * 255.0 /
            (BFPLAYER_HDR_CHROMA_LEVELS - 1)));
        const double u = bfplayer::decode_chroma_code(
            u_code,
            config->input_full_range != 0);
        for (int v_level = 0;
             v_level < BFPLAYER_HDR_CHROMA_LEVELS;
             ++v_level) {
            const int v_code = static_cast<int>(std::lround(
                static_cast<double>(v_level) * 255.0 /
                (BFPLAYER_HDR_CHROMA_LEVELS - 1)));
            const double v = bfplayer::decode_chroma_code(
                v_code,
                config->input_full_range != 0);
            for (int y_code = 0;
                 y_code < BFPLAYER_HDR_LUMA_LEVELS;
                 ++y_code) {
                const double y = bfplayer::decode_luma_code(
                    y_code,
                    config->input_full_range != 0);
                const bfplayer::RgbTriplet input_rgb =
                    bfplayer::decode_yuv(
                        y,
                        u,
                        v,
                        config->input_bt2020 != 0);
                const bfplayer::RgbTriplet output_rgb =
                    bfplayer::tone_map_hdr_to_bt709_sdr(
                        input_rgb,
                        settings,
                        config->input_bt2020 != 0);
                const bfplayer::RgbTriplet output_yuv =
                    bfplayer::encode_bt709_yuv(output_rgb);
                BfplayerHdrYuvEntry& entry =
                    lut->entries[bfplayer::yuv_lut_index(
                        y_code,
                        u_level,
                        v_level)];
                const std::uint32_t output_y =
                    bfplayer::encode_limited_luma(output_yuv.red);
                const std::uint32_t output_u =
                    bfplayer::encode_limited_chroma(output_yuv.green);
                const std::uint32_t output_v =
                    bfplayer::encode_limited_chroma(output_yuv.blue);
                entry.packed =
                    output_y |
                    (output_u << 8U) |
                    (output_v << 16U);
            }
        }
    }
    return 0;
}

extern "C" int bfplayer_apply_hdr_yuv420_lut(
    const BfplayerHdrYuvLut* lut,
    std::uint8_t* y_plane,
    int y_stride,
    std::uint8_t* u_plane,
    int u_stride,
    std::uint8_t* v_plane,
    int v_stride,
    int width,
    int height) {
    if (!lut || !y_plane || !u_plane || !v_plane ||
        width <= 0 || height <= 0 ||
        y_stride < width ||
        u_stride < (width + 1) / 2 ||
        v_stride < (width + 1) / 2) {
        return -1;
    }

    const int chroma_height = (height + 1) / 2;
    bfplayer::apply_hdr_yuv420_rows(
        lut,
        y_plane,
        y_stride,
        u_plane,
        u_stride,
        v_plane,
        v_stride,
        width,
        height,
        0,
        chroma_height);
    return 0;
}

#if defined(BFPLAYER_PS5)

namespace {

constexpr unsigned int kMaximumHdrWorkers = 11;

struct HdrWorkerArgument;

} // namespace

struct BfplayerHdrWorkerPool {
    pthread_mutex_t mutex{};
    pthread_cond_t work_ready{};
    pthread_cond_t work_done{};
    pthread_t threads[kMaximumHdrWorkers]{};
    HdrWorkerArgument* arguments = nullptr;
    unsigned int worker_count = 0;
    unsigned int completed = 0;
    unsigned long long generation = 0;
    bool stopping = false;
    const BfplayerHdrYuvLut* lut = nullptr;
    std::uint8_t* y_plane = nullptr;
    int y_stride = 0;
    std::uint8_t* u_plane = nullptr;
    int u_stride = 0;
    std::uint8_t* v_plane = nullptr;
    int v_stride = 0;
    int width = 0;
    int height = 0;
};

namespace {

struct HdrWorkerArgument {
    BfplayerHdrWorkerPool* pool = nullptr;
    unsigned int segment = 0;
};

void apply_hdr_worker_segment(
    BfplayerHdrWorkerPool* pool,
    unsigned int segment) {
    const int chroma_height = (pool->height + 1) / 2;
    const unsigned int segments = pool->worker_count + 1;
    const int begin = static_cast<int>(
        static_cast<long long>(chroma_height) * segment / segments);
    const int end = static_cast<int>(
        static_cast<long long>(chroma_height) * (segment + 1) / segments);
    bfplayer::apply_hdr_yuv420_rows(
        pool->lut,
        pool->y_plane,
        pool->y_stride,
        pool->u_plane,
        pool->u_stride,
        pool->v_plane,
        pool->v_stride,
        pool->width,
        pool->height,
        begin,
        end);
}

void* hdr_worker_main(void* opaque) {
    auto* argument = static_cast<HdrWorkerArgument*>(opaque);
    BfplayerHdrWorkerPool* pool = argument->pool;
    unsigned long long observed_generation = 0;
    pthread_mutex_lock(&pool->mutex);
    for (;;) {
        while (!pool->stopping &&
               pool->generation == observed_generation) {
            pthread_cond_wait(&pool->work_ready, &pool->mutex);
        }
        if (pool->stopping) {
            pthread_mutex_unlock(&pool->mutex);
            return nullptr;
        }
        observed_generation = pool->generation;
        pthread_mutex_unlock(&pool->mutex);
        apply_hdr_worker_segment(pool, argument->segment);
        pthread_mutex_lock(&pool->mutex);
        ++pool->completed;
        if (pool->completed == pool->worker_count) {
            pthread_cond_signal(&pool->work_done);
        }
    }
}

} // namespace

extern "C" BfplayerHdrWorkerPool* bfplayer_create_hdr_worker_pool(
    unsigned int worker_count) {
    worker_count = std::min(worker_count, kMaximumHdrWorkers);
    auto* pool = static_cast<BfplayerHdrWorkerPool*>(
        std::calloc(1, sizeof(BfplayerHdrWorkerPool)));
    if (!pool) {
        return nullptr;
    }
    if (pthread_mutex_init(&pool->mutex, nullptr) != 0) {
        std::free(pool);
        return nullptr;
    }
    if (pthread_cond_init(&pool->work_ready, nullptr) != 0) {
        pthread_mutex_destroy(&pool->mutex);
        std::free(pool);
        return nullptr;
    }
    if (pthread_cond_init(&pool->work_done, nullptr) != 0) {
        pthread_cond_destroy(&pool->work_ready);
        pthread_mutex_destroy(&pool->mutex);
        std::free(pool);
        return nullptr;
    }
    pool->arguments = static_cast<HdrWorkerArgument*>(
        std::calloc(worker_count, sizeof(HdrWorkerArgument)));
    if (worker_count > 0 && !pool->arguments) {
        pthread_cond_destroy(&pool->work_done);
        pthread_cond_destroy(&pool->work_ready);
        pthread_mutex_destroy(&pool->mutex);
        std::free(pool);
        return nullptr;
    }
    for (unsigned int index = 0; index < worker_count; ++index) {
        pool->arguments[index].pool = pool;
        pool->arguments[index].segment = index + 1;
        if (pthread_create(
                &pool->threads[index],
                nullptr,
                hdr_worker_main,
                &pool->arguments[index]) != 0) {
            break;
        }
        ++pool->worker_count;
    }
    return pool;
}

extern "C" void bfplayer_destroy_hdr_worker_pool(
    BfplayerHdrWorkerPool* pool) {
    if (!pool) {
        return;
    }
    pthread_mutex_lock(&pool->mutex);
    pool->stopping = true;
    pthread_cond_broadcast(&pool->work_ready);
    pthread_mutex_unlock(&pool->mutex);
    for (unsigned int index = 0; index < pool->worker_count; ++index) {
        pthread_join(pool->threads[index], nullptr);
    }
    std::free(pool->arguments);
    pthread_cond_destroy(&pool->work_done);
    pthread_cond_destroy(&pool->work_ready);
    pthread_mutex_destroy(&pool->mutex);
    std::free(pool);
}

extern "C" unsigned int bfplayer_hdr_worker_count(
    const BfplayerHdrWorkerPool* pool) {
    return pool ? pool->worker_count : 0;
}

extern "C" int bfplayer_apply_hdr_yuv420_lut_parallel(
    BfplayerHdrWorkerPool* pool,
    const BfplayerHdrYuvLut* lut,
    std::uint8_t* y_plane,
    int y_stride,
    std::uint8_t* u_plane,
    int u_stride,
    std::uint8_t* v_plane,
    int v_stride,
    int width,
    int height) {
    if (!pool || pool->worker_count == 0) {
        return bfplayer_apply_hdr_yuv420_lut(
            lut,
            y_plane,
            y_stride,
            u_plane,
            u_stride,
            v_plane,
            v_stride,
            width,
            height);
    }
    if (!lut || !y_plane || !u_plane || !v_plane ||
        width <= 0 || height <= 0 ||
        y_stride < width ||
        u_stride < (width + 1) / 2 ||
        v_stride < (width + 1) / 2) {
        return -1;
    }
    pthread_mutex_lock(&pool->mutex);
    pool->lut = lut;
    pool->y_plane = y_plane;
    pool->y_stride = y_stride;
    pool->u_plane = u_plane;
    pool->u_stride = u_stride;
    pool->v_plane = v_plane;
    pool->v_stride = v_stride;
    pool->width = width;
    pool->height = height;
    pool->completed = 0;
    ++pool->generation;
    pthread_cond_broadcast(&pool->work_ready);
    pthread_mutex_unlock(&pool->mutex);

    apply_hdr_worker_segment(pool, 0);

    pthread_mutex_lock(&pool->mutex);
    while (pool->completed < pool->worker_count) {
        pthread_cond_wait(&pool->work_done, &pool->mutex);
    }
    pthread_mutex_unlock(&pool->mutex);
    return 0;
}

#else

struct BfplayerHdrWorkerPool {};

extern "C" BfplayerHdrWorkerPool* bfplayer_create_hdr_worker_pool(
    unsigned int) {
    return nullptr;
}

extern "C" void bfplayer_destroy_hdr_worker_pool(
    BfplayerHdrWorkerPool*) {}

extern "C" unsigned int bfplayer_hdr_worker_count(
    const BfplayerHdrWorkerPool*) {
    return 0;
}

extern "C" int bfplayer_apply_hdr_yuv420_lut_parallel(
    BfplayerHdrWorkerPool*,
    const BfplayerHdrYuvLut* lut,
    std::uint8_t* y_plane,
    int y_stride,
    std::uint8_t* u_plane,
    int u_stride,
    std::uint8_t* v_plane,
    int v_stride,
    int width,
    int height) {
    return bfplayer_apply_hdr_yuv420_lut(
        lut,
        y_plane,
        y_stride,
        u_plane,
        u_stride,
        v_plane,
        v_stride,
        width,
        height);
}

#endif
