#include "bfplayer/hdr_tonemap.hpp"
#include "bfplayer/hdr_yuv_tonemap.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

namespace {

bool near(double first, double second, double tolerance = 1.0e-8) {
    return std::abs(first - second) <= tolerance;
}

void assert_unit(const bfplayer::RgbTriplet& value) {
    assert(std::isfinite(value.red));
    assert(std::isfinite(value.green));
    assert(std::isfinite(value.blue));
    assert(value.red >= 0.0 && value.red <= 1.0);
    assert(value.green >= 0.0 && value.green <= 1.0);
    assert(value.blue >= 0.0 && value.blue <= 1.0);
}

} // namespace

int main() {
    using bfplayer::HdrToneMapSettings;
    using bfplayer::HdrTransfer;
    using bfplayer::RgbTriplet;

    const HdrToneMapSettings pq{
        HdrTransfer::pq,
        1000.0,
        100.0,
        17,
    };
    const RgbTriplet black =
        bfplayer::tone_map_bt2020_hdr_to_bt709_sdr({}, pq);
    assert(near(black.red, 0.0));
    assert(near(black.green, 0.0));
    assert(near(black.blue, 0.0));

    double previous_neutral = 0.0;
    for (int index = 0; index <= 100; ++index) {
        const double encoded = index / 100.0;
        const RgbTriplet mapped =
            bfplayer::tone_map_bt2020_hdr_to_bt709_sdr(
                {encoded, encoded, encoded},
                pq);
        assert_unit(mapped);
        assert(near(mapped.red, mapped.green, 2.0e-5));
        assert(near(mapped.green, mapped.blue, 2.0e-5));
        assert(mapped.red + 1.0e-9 >= previous_neutral);
        previous_neutral = mapped.red;
    }
    assert(previous_neutral > 0.99);

    const RgbTriplet saturated =
        bfplayer::tone_map_bt2020_hdr_to_bt709_sdr(
            {1.0, 0.25, 0.0},
            pq);
    assert_unit(saturated);
    assert(saturated.red >= saturated.green);
    assert(saturated.red >= saturated.blue);
    assert(saturated.red > 0.1);

    const HdrToneMapSettings hlg{
        HdrTransfer::hlg,
        1000.0,
        100.0,
        17,
    };
    previous_neutral = 0.0;
    for (int index = 0; index <= 100; ++index) {
        const double encoded = index / 100.0;
        const RgbTriplet mapped =
            bfplayer::tone_map_bt2020_hdr_to_bt709_sdr(
                {encoded, encoded, encoded},
                hlg);
        assert_unit(mapped);
        assert(near(mapped.red, mapped.green, 2.0e-5));
        assert(near(mapped.green, mapped.blue, 2.0e-5));
        assert(mapped.red + 1.0e-9 >= previous_neutral);
        previous_neutral = mapped.red;
    }
    assert(previous_neutral > 0.99);

    std::string cube;
    std::string error;
    assert(bfplayer::make_hdr_tonemap_cube(pq, cube, error));
    assert(error.empty());
    assert(cube.find("LUT_3D_SIZE 17\n") != std::string::npos);
    assert(cube.find("DOMAIN_MIN 0.0 0.0 0.0\n") != std::string::npos);
    assert(
        static_cast<int>(std::count(cube.begin(), cube.end(), '\n')) ==
        4 + 17 * 17 * 17);

    HdrToneMapSettings invalid = pq;
    invalid.source_peak_nits = 0.0;
    assert(!bfplayer::make_hdr_tonemap_cube(invalid, cube, error));
    assert(!error.empty());

    const std::string temporary = "bfplayer-hdr-tonemap-test.cube";
    assert(bfplayer::write_hdr_tonemap_cube_atomic(
        temporary,
        pq,
        error));
    std::ifstream written(temporary, std::ios::binary);
    assert(written.good());
    const std::string contents{
        std::istreambuf_iterator<char>(written),
        std::istreambuf_iterator<char>(),
    };
    written.close();
    assert(contents.find("BFplayer PQ") != std::string::npos);
    assert(std::remove(temporary.c_str()) == 0);

    auto yuv_lut = std::make_unique<BfplayerHdrYuvLut>();
    BfplayerHdrYuvConfig yuv_config{
        BFPLAYER_HDR_TRANSFER_PQ,
        1,
        1,
        1000.0,
        100.0,
    };
    assert(bfplayer_build_hdr_yuv420_lut(
        &yuv_config,
        yuv_lut.get()) == 0);
    assert(yuv_lut->config.transfer == BFPLAYER_HDR_TRANSFER_PQ);

    constexpr int width = 4;
    constexpr int height = 2;
    constexpr int y_stride = 6;
    constexpr int chroma_stride = 4;
    std::array<std::uint8_t, y_stride * height> y_plane{
        0, 64, 128, 255, 0xee, 0xee,
        16, 96, 160, 235, 0xee, 0xee,
    };
    std::array<std::uint8_t, chroma_stride> u_plane{
        128, 128, 0xee, 0xee,
    };
    std::array<std::uint8_t, chroma_stride> v_plane{
        128, 128, 0xee, 0xee,
    };
    assert(bfplayer_apply_hdr_yuv420_lut(
        yuv_lut.get(),
        y_plane.data(),
        y_stride,
        u_plane.data(),
        chroma_stride,
        v_plane.data(),
        chroma_stride,
        width,
        height) == 0);
    assert(y_plane[0] == 16);
    assert(y_plane[3] == 235);
    assert(y_plane[4] == 0xee);
    assert(y_plane[10] == 0xee);
    assert(y_plane[1] < y_plane[2]);
    assert(y_plane[2] < y_plane[3]);
    assert(u_plane[0] >= 126 && u_plane[0] <= 130);
    assert(v_plane[0] >= 126 && v_plane[0] <= 130);
    assert(u_plane[2] == 0xee);
    assert(v_plane[2] == 0xee);

    BfplayerHdrYuvConfig invalid_yuv = yuv_config;
    invalid_yuv.source_peak_nits = 0.0;
    assert(bfplayer_build_hdr_yuv420_lut(
        &invalid_yuv,
        yuv_lut.get()) != 0);

    constexpr int benchmark_width = 3840;
    constexpr int benchmark_height = 2160;
    const std::size_t benchmark_luma_size =
        static_cast<std::size_t>(benchmark_width) * benchmark_height;
    const std::size_t benchmark_chroma_size =
        static_cast<std::size_t>(benchmark_width / 2) *
        (benchmark_height / 2);
    auto benchmark_y =
        std::make_unique<std::uint8_t[]>(benchmark_luma_size);
    auto benchmark_u =
        std::make_unique<std::uint8_t[]>(benchmark_chroma_size);
    auto benchmark_v =
        std::make_unique<std::uint8_t[]>(benchmark_chroma_size);
    std::fill_n(benchmark_y.get(), benchmark_luma_size, 128);
    std::fill_n(benchmark_u.get(), benchmark_chroma_size, 128);
    std::fill_n(benchmark_v.get(), benchmark_chroma_size, 128);
    const auto benchmark_started = std::chrono::steady_clock::now();
    assert(bfplayer_apply_hdr_yuv420_lut(
        yuv_lut.get(),
        benchmark_y.get(),
        benchmark_width,
        benchmark_u.get(),
        benchmark_width / 2,
        benchmark_v.get(),
        benchmark_width / 2,
        benchmark_width,
        benchmark_height) == 0);
    const auto benchmark_elapsed =
        std::chrono::steady_clock::now() - benchmark_started;
    assert(
        std::chrono::duration_cast<std::chrono::seconds>(
            benchmark_elapsed).count() < 2);

    return 0;
}
