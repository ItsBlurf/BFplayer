#include "bfplayer/video_layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace bfplayer {
namespace {

template <typename Mode, std::size_t Size>
struct RatioEntry {
    Mode mode{};
    const char* key = nullptr;
    const char* name = nullptr;
    double ratio = 0.0;
};

constexpr std::array<RatioEntry<VideoAspectMode, 9>, 9> kAspectModes{{
    {VideoAspectMode::default_ratio, "default", "Original", 0.0},
    {VideoAspectMode::ratio_1_1, "1:1", "1:1", 1.0},
    {VideoAspectMode::ratio_4_3, "4:3", "4:3", 4.0 / 3.0},
    {VideoAspectMode::ratio_16_9, "16:9", "16:9", 16.0 / 9.0},
    {VideoAspectMode::ratio_16_10, "16:10", "16:10", 16.0 / 10.0},
    {VideoAspectMode::ratio_2_21_1, "2.21:1", "2.21:1", 2.21},
    {VideoAspectMode::ratio_2_35_1, "2.35:1", "2.35:1", 2.35},
    {VideoAspectMode::ratio_2_39_1, "2.39:1", "2.39:1", 2.39},
    {VideoAspectMode::ratio_5_4, "5:4", "5:4", 5.0 / 4.0},
}};

constexpr std::array<RatioEntry<VideoCropMode, 11>, 11> kCropModes{{
    {VideoCropMode::default_crop, "default", "Default", 0.0},
    {VideoCropMode::ratio_16_10, "16:10", "16:10", 16.0 / 10.0},
    {VideoCropMode::ratio_16_9, "16:9", "16:9", 16.0 / 9.0},
    {VideoCropMode::ratio_1_85_1, "1.85:1", "1.85:1", 1.85},
    {VideoCropMode::ratio_2_21_1, "2.21:1", "2.21:1", 2.21},
    {VideoCropMode::ratio_2_35_1, "2.35:1", "2.35:1", 2.35},
    {VideoCropMode::ratio_2_39_1, "2.39:1", "2.39:1", 2.39},
    {VideoCropMode::ratio_5_3, "5:3", "5:3", 5.0 / 3.0},
    {VideoCropMode::ratio_4_3, "4:3", "4:3", 4.0 / 3.0},
    {VideoCropMode::ratio_5_4, "5:4", "5:4", 5.0 / 4.0},
    {VideoCropMode::ratio_1_1, "1:1", "1:1", 1.0},
}};

template <typename Mode, std::size_t Size>
const RatioEntry<Mode, Size>& find_entry(
    Mode mode,
    const std::array<RatioEntry<Mode, Size>, Size>& entries) noexcept {
    const auto found = std::find_if(
        entries.begin(), entries.end(),
        [mode](const auto& entry) { return entry.mode == mode; });
    return found == entries.end() ? entries.front() : *found;
}

template <typename Mode, std::size_t Size>
std::optional<Mode> parse_ratio_mode(
    std::string_view value,
    const std::array<RatioEntry<Mode, Size>, Size>& entries) noexcept {
    const auto found = std::find_if(
        entries.begin(), entries.end(),
        [value](const auto& entry) { return value == entry.key; });
    return found == entries.end() ? std::nullopt : std::optional<Mode>(found->mode);
}

template <typename Mode, std::size_t Size>
Mode next_ratio_mode(
    Mode mode,
    const std::array<RatioEntry<Mode, Size>, Size>& entries) noexcept {
    const auto found = std::find_if(
        entries.begin(), entries.end(),
        [mode](const auto& entry) { return entry.mode == mode; });
    if (found == entries.end() || std::next(found) == entries.end()) {
        return entries.front().mode;
    }
    return std::next(found)->mode;
}

template <typename Mode, std::size_t Size>
Mode step_ratio_mode(
    Mode mode,
    int direction,
    const std::array<RatioEntry<Mode, Size>, Size>& entries) noexcept {
    const auto found = std::find_if(
        entries.begin(), entries.end(),
        [mode](const auto& entry) { return entry.mode == mode; });
    if (found == entries.end()) {
        return entries.front().mode;
    }
    const std::size_t index =
        static_cast<std::size_t>(found - entries.begin());
    const std::size_t next_index = direction < 0
        ? (index + entries.size() - 1) % entries.size()
        : (index + 1) % entries.size();
    return entries[next_index].mode;
}

double valid_stream_aspect(
    int frame_width,
    int frame_height,
    double display_aspect) noexcept {
    if (!(display_aspect > 0.0) || !std::isfinite(display_aspect)) {
        return static_cast<double>(frame_width) /
               static_cast<double>(frame_height);
    }
    return display_aspect;
}

void crop_rect_to_ratio(
    VideoRect& source,
    double current_display_aspect,
    double target_display_aspect) noexcept {
    if (!(current_display_aspect > 0.0) ||
        !(target_display_aspect > 0.0) ||
        !std::isfinite(current_display_aspect) ||
        !std::isfinite(target_display_aspect)) {
        return;
    }
    if (current_display_aspect > target_display_aspect) {
        const int width = std::clamp(
            static_cast<int>(std::llround(
                source.width * target_display_aspect / current_display_aspect)),
            1,
            source.width);
        source.x += (source.width - width) / 2;
        source.width = width;
    } else if (current_display_aspect < target_display_aspect) {
        const int height = std::clamp(
            static_cast<int>(std::llround(
                source.height * current_display_aspect / target_display_aspect)),
            1,
            source.height);
        source.y += (source.height - height) / 2;
        source.height = height;
    }
}

} // namespace

const char* video_scale_mode_name(VideoScaleMode mode) noexcept {
    switch (mode) {
        case VideoScaleMode::fill:
            return "Fill screen (crop)";
        case VideoScaleMode::stretch:
            return "Fullscreen stretch";
        default:
            return "Best fit";
    }
}

const char* video_scale_mode_key(VideoScaleMode mode) noexcept {
    switch (mode) {
        case VideoScaleMode::fill:
            return "fill";
        case VideoScaleMode::stretch:
            return "stretch";
        default:
            return "fit";
    }
}

std::optional<VideoScaleMode> parse_video_scale_mode(
    std::string_view value) noexcept {
    if (value == "fit") {
        return VideoScaleMode::fit;
    }
    if (value == "fill") {
        return VideoScaleMode::fill;
    }
    if (value == "stretch") {
        return VideoScaleMode::stretch;
    }
    return std::nullopt;
}

VideoScaleMode next_video_scale_mode(VideoScaleMode mode) noexcept {
    switch (mode) {
        case VideoScaleMode::fit:
            return VideoScaleMode::fill;
        case VideoScaleMode::fill:
            return VideoScaleMode::stretch;
        default:
            return VideoScaleMode::fit;
    }
}

VideoScaleMode step_video_scale_mode(
    VideoScaleMode mode,
    int direction) noexcept {
    constexpr std::array<VideoScaleMode, 3> modes{
        VideoScaleMode::fit,
        VideoScaleMode::fill,
        VideoScaleMode::stretch,
    };
    const auto found = std::find(modes.begin(), modes.end(), mode);
    if (found == modes.end()) {
        return modes.front();
    }
    const std::size_t index =
        static_cast<std::size_t>(found - modes.begin());
    const std::size_t next_index = direction < 0
        ? (index + modes.size() - 1) % modes.size()
        : (index + 1) % modes.size();
    return modes[next_index];
}

const char* video_aspect_mode_name(VideoAspectMode mode) noexcept {
    return find_entry(mode, kAspectModes).name;
}

const char* video_aspect_mode_key(VideoAspectMode mode) noexcept {
    return find_entry(mode, kAspectModes).key;
}

std::optional<VideoAspectMode> parse_video_aspect_mode(
    std::string_view value) noexcept {
    return parse_ratio_mode(value, kAspectModes);
}

VideoAspectMode next_video_aspect_mode(VideoAspectMode mode) noexcept {
    return next_ratio_mode(mode, kAspectModes);
}

VideoAspectMode step_video_aspect_mode(
    VideoAspectMode mode,
    int direction) noexcept {
    return step_ratio_mode(mode, direction, kAspectModes);
}

const char* video_crop_mode_name(VideoCropMode mode) noexcept {
    return find_entry(mode, kCropModes).name;
}

const char* video_crop_mode_key(VideoCropMode mode) noexcept {
    return find_entry(mode, kCropModes).key;
}

std::optional<VideoCropMode> parse_video_crop_mode(
    std::string_view value) noexcept {
    return parse_ratio_mode(value, kCropModes);
}

VideoCropMode next_video_crop_mode(VideoCropMode mode) noexcept {
    return next_ratio_mode(mode, kCropModes);
}

VideoCropMode step_video_crop_mode(
    VideoCropMode mode,
    int direction) noexcept {
    return step_ratio_mode(mode, direction, kCropModes);
}

double display_aspect_from_sample_aspect(
    int frame_width,
    int frame_height,
    int sample_aspect_numerator,
    int sample_aspect_denominator) noexcept {
    if (frame_width <= 0 || frame_height <= 0) {
        return 0.0;
    }
    const double frame_aspect =
        static_cast<double>(frame_width) / static_cast<double>(frame_height);
    if (sample_aspect_numerator <= 0 || sample_aspect_denominator <= 0) {
        return frame_aspect;
    }
    const double display_aspect =
        frame_aspect *
        static_cast<double>(sample_aspect_numerator) /
        static_cast<double>(sample_aspect_denominator);
    return std::isfinite(display_aspect) && display_aspect > 0.0
        ? display_aspect
        : frame_aspect;
}

VideoLayout compute_video_layout(
    int frame_width,
    int frame_height,
    double display_aspect,
    int output_width,
    int output_height,
    VideoScaleMode mode) noexcept {
    return compute_video_layout(
        frame_width,
        frame_height,
        display_aspect,
        output_width,
        output_height,
        mode,
        VideoAspectMode::default_ratio,
        VideoCropMode::default_crop);
}

VideoLayout compute_video_layout(
    int frame_width,
    int frame_height,
    double display_aspect,
    int output_width,
    int output_height,
    VideoScaleMode scale_mode,
    VideoAspectMode aspect_mode,
    VideoCropMode crop_mode) noexcept {
    VideoLayout layout{};
    if (frame_width <= 0 || frame_height <= 0 ||
        output_width <= 0 || output_height <= 0) {
        return layout;
    }

    layout.source = {0, 0, frame_width, frame_height};
    layout.destination = {0, 0, output_width, output_height};
    display_aspect = valid_stream_aspect(
        frame_width, frame_height, display_aspect);
    const double crop_aspect = find_entry(crop_mode, kCropModes).ratio;
    double effective_aspect = display_aspect;
    if (crop_aspect > 0.0) {
        crop_rect_to_ratio(layout.source, display_aspect, crop_aspect);
        effective_aspect = crop_aspect;
    }
    const double aspect_override = find_entry(aspect_mode, kAspectModes).ratio;
    if (aspect_override > 0.0) {
        effective_aspect = aspect_override;
    }

    if (scale_mode == VideoScaleMode::stretch) {
        layout.crop_source =
            layout.source.x != 0 || layout.source.y != 0 ||
            layout.source.width != frame_width ||
            layout.source.height != frame_height;
        return layout;
    }

    const double output_aspect =
        static_cast<double>(output_width) / static_cast<double>(output_height);

    if (scale_mode == VideoScaleMode::fit) {
        if (effective_aspect >= output_aspect) {
            const int height = std::clamp(
                static_cast<int>(std::llround(output_width / effective_aspect)),
                1,
                output_height);
            layout.destination = {
                0,
                (output_height - height) / 2,
                output_width,
                height};
        } else {
            const int width = std::clamp(
                static_cast<int>(std::llround(output_height * effective_aspect)),
                1,
                output_width);
            layout.destination = {
                (output_width - width) / 2,
                0,
                width,
                output_height};
        }
    } else {
        crop_rect_to_ratio(layout.source, effective_aspect, output_aspect);
        layout.destination = {0, 0, output_width, output_height};
    }
    layout.crop_source =
        layout.source.x != 0 || layout.source.y != 0 ||
        layout.source.width != frame_width ||
        layout.source.height != frame_height;
    return layout;
}

} // namespace bfplayer
