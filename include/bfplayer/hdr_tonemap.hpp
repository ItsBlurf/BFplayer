#pragma once

#include <string>

namespace bfplayer {

enum class HdrTransfer {
    pq,
    hlg,
};

struct HdrToneMapSettings {
    HdrTransfer transfer = HdrTransfer::pq;
    double source_peak_nits = 1000.0;
    double target_peak_nits = 100.0;
    int cube_size = 17;
};

struct RgbTriplet {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

RgbTriplet tone_map_bt2020_hdr_to_bt709_sdr(
    RgbTriplet encoded_bt2020,
    const HdrToneMapSettings& settings);

bool make_hdr_tonemap_cube(
    const HdrToneMapSettings& settings,
    std::string& cube,
    std::string& error);

bool write_hdr_tonemap_cube_atomic(
    const std::string& path,
    const HdrToneMapSettings& settings,
    std::string& error);

} // namespace bfplayer
