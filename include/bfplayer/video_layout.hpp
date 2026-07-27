#pragma once

#include <optional>
#include <string_view>

namespace bfplayer {

enum class VideoScaleMode {
    fit = 0,
    fill,
    stretch,
};

// VLC Desktop exposes display-aspect overrides separately from crop ratios.
// "Original" keeps the stream's display aspect (including pixel-aspect data).
enum class VideoAspectMode {
    default_ratio = 0,
    ratio_1_1,
    ratio_4_3,
    ratio_16_9,
    ratio_16_10,
    ratio_2_21_1,
    ratio_2_35_1,
    ratio_2_39_1,
    ratio_5_4,
};

enum class VideoCropMode {
    default_crop = 0,
    ratio_16_10,
    ratio_16_9,
    ratio_1_85_1,
    ratio_2_21_1,
    ratio_2_35_1,
    ratio_2_39_1,
    ratio_5_3,
    ratio_4_3,
    ratio_5_4,
    ratio_1_1,
};

struct VideoRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct VideoLayout {
    VideoRect source;
    VideoRect destination;
    bool crop_source = false;
};

[[nodiscard]] const char* video_scale_mode_name(VideoScaleMode mode) noexcept;
[[nodiscard]] const char* video_scale_mode_key(VideoScaleMode mode) noexcept;
[[nodiscard]] std::optional<VideoScaleMode> parse_video_scale_mode(
    std::string_view value) noexcept;
[[nodiscard]] VideoScaleMode next_video_scale_mode(VideoScaleMode mode) noexcept;
[[nodiscard]] VideoScaleMode step_video_scale_mode(
    VideoScaleMode mode,
    int direction) noexcept;

[[nodiscard]] const char* video_aspect_mode_name(VideoAspectMode mode) noexcept;
[[nodiscard]] const char* video_aspect_mode_key(VideoAspectMode mode) noexcept;
[[nodiscard]] std::optional<VideoAspectMode> parse_video_aspect_mode(
    std::string_view value) noexcept;
[[nodiscard]] VideoAspectMode next_video_aspect_mode(VideoAspectMode mode) noexcept;
[[nodiscard]] VideoAspectMode step_video_aspect_mode(
    VideoAspectMode mode,
    int direction) noexcept;

[[nodiscard]] const char* video_crop_mode_name(VideoCropMode mode) noexcept;
[[nodiscard]] const char* video_crop_mode_key(VideoCropMode mode) noexcept;
[[nodiscard]] std::optional<VideoCropMode> parse_video_crop_mode(
    std::string_view value) noexcept;
[[nodiscard]] VideoCropMode next_video_crop_mode(VideoCropMode mode) noexcept;
[[nodiscard]] VideoCropMode step_video_crop_mode(
    VideoCropMode mode,
    int direction) noexcept;

// Converts decoded frame dimensions plus a sample/pixel aspect ratio (SAR)
// into the final display aspect ratio (DAR). Invalid SAR falls back to square
// pixels, so a 1920x1080 frame remains 16:9 rather than becoming 1:1.
[[nodiscard]] double display_aspect_from_sample_aspect(
    int frame_width,
    int frame_height,
    int sample_aspect_numerator,
    int sample_aspect_denominator) noexcept;

// display_aspect is the decoded stream's display aspect ratio after pixel-aspect
// correction. A non-positive value falls back to frame_width/frame_height.
[[nodiscard]] VideoLayout compute_video_layout(
    int frame_width,
    int frame_height,
    double display_aspect,
    int output_width,
    int output_height,
    VideoScaleMode mode) noexcept;

// Applies operations in the same conceptual order as VLC: crop the decoded
// picture, optionally override its display aspect, then fit/fill/stretch it.
[[nodiscard]] VideoLayout compute_video_layout(
    int frame_width,
    int frame_height,
    double display_aspect,
    int output_width,
    int output_height,
    VideoScaleMode scale_mode,
    VideoAspectMode aspect_mode,
    VideoCropMode crop_mode) noexcept;

} // namespace bfplayer
