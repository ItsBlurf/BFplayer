#include "ps5mc/video_layout.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool rect_is(
    const ps5mc::VideoRect& rect,
    int x,
    int y,
    int width,
    int height) {
    return rect.x == x && rect.y == y &&
           rect.width == width && rect.height == height;
}

} // namespace

int main() {
    using ps5mc::VideoScaleMode;
    using ps5mc::VideoAspectMode;
    using ps5mc::VideoCropMode;

    const auto fit_4_3 = ps5mc::compute_video_layout(
        1440, 1080, 4.0 / 3.0, 1920, 1080, VideoScaleMode::fit);
    check(!fit_4_3.crop_source, "fit does not crop");
    check(rect_is(fit_4_3.destination, 240, 0, 1440, 1080), "4:3 fit pillarbox");

    const auto fill_4_3 = ps5mc::compute_video_layout(
        1440, 1080, 4.0 / 3.0, 1920, 1080, VideoScaleMode::fill);
    check(fill_4_3.crop_source, "fill crops");
    check(rect_is(fill_4_3.source, 0, 135, 1440, 810), "4:3 fill vertical crop");
    check(rect_is(fill_4_3.destination, 0, 0, 1920, 1080), "fill covers output");

    const auto fit_wide = ps5mc::compute_video_layout(
        1920, 817, 2.35, 1920, 1080, VideoScaleMode::fit);
    check(rect_is(fit_wide.destination, 0, 131, 1920, 817), "wide fit letterbox");

    const auto fill_wide = ps5mc::compute_video_layout(
        1920, 817, 2.35, 1920, 1080, VideoScaleMode::fill);
    check(rect_is(fill_wide.source, 234, 0, 1452, 817), "wide fill horizontal crop");

    const auto anamorphic = ps5mc::compute_video_layout(
        720, 480, 16.0 / 9.0, 1920, 1080, VideoScaleMode::fit);
    check(rect_is(anamorphic.destination, 0, 0, 1920, 1080), "display aspect honored");
    const auto anamorphic_crop = ps5mc::compute_video_layout(
        720, 480, 16.0 / 9.0, 1920, 1080,
        VideoScaleMode::fit,
        VideoAspectMode::default_ratio,
        VideoCropMode::ratio_4_3);
    check(
        rect_is(anamorphic_crop.source, 90, 0, 540, 480),
        "anamorphic crop accounts for pixel aspect");
    check(
        rect_is(anamorphic_crop.destination, 240, 0, 1440, 1080),
        "anamorphic 4:3 crop remains exact at output");

    const auto stretch = ps5mc::compute_video_layout(
        640, 480, 4.0 / 3.0, 1920, 1080, VideoScaleMode::stretch);
    check(!stretch.crop_source, "stretch uses full source");
    check(rect_is(stretch.destination, 0, 0, 1920, 1080), "stretch covers output");

    const auto fallback = ps5mc::compute_video_layout(
        1920, 1080, 0.0, 1280, 720, VideoScaleMode::fit);
    check(rect_is(fallback.destination, 0, 0, 1280, 720), "invalid aspect fallback");
    const auto fallback_4_3 = ps5mc::compute_video_layout(
        640, 480, 0.0, 1920, 1080, VideoScaleMode::fit);
    check(
        rect_is(fallback_4_3.destination, 240, 0, 1440, 1080),
        "missing display aspect falls back to original frame ratio");
    const auto infinite_aspect = ps5mc::compute_video_layout(
        1920,
        1080,
        std::numeric_limits<double>::infinity(),
        1280,
        720,
        VideoScaleMode::fit);
    check(rect_is(infinite_aspect.destination, 0, 0, 1280, 720),
          "infinite aspect uses frame fallback");
    const auto tiny_aspect = ps5mc::compute_video_layout(
        1920,
        1080,
        std::numeric_limits<double>::min(),
        1280,
        720,
        VideoScaleMode::fill);
    check(tiny_aspect.source.width > 0 && tiny_aspect.source.height > 0,
          "extreme finite aspect remains bounded");
    const auto invalid_dimensions = ps5mc::compute_video_layout(
        0, 1080, 16.0 / 9.0, 1280, 720, VideoScaleMode::fit);
    check(rect_is(invalid_dimensions.destination, 0, 0, 0, 0),
          "invalid dimensions return an empty layout");

    check(
        ps5mc::parse_video_scale_mode("fill") == VideoScaleMode::fill,
        "parse scale mode");
    check(!ps5mc::parse_video_scale_mode("crop").has_value(), "reject unknown mode");
    check(
        ps5mc::next_video_scale_mode(VideoScaleMode::stretch) == VideoScaleMode::fit,
        "scale mode wraps");
    check(
        ps5mc::step_video_scale_mode(VideoScaleMode::fit, -1) ==
            VideoScaleMode::stretch,
        "scale mode steps backward and wraps");
    check(
        std::string(ps5mc::video_scale_mode_name(VideoScaleMode::fill)) ==
            "Fill screen (crop)",
        "scale mode display name");

    const auto aspect_4_3 = ps5mc::compute_video_layout(
        1920, 1080, 16.0 / 9.0, 1920, 1080,
        VideoScaleMode::fit,
        VideoAspectMode::ratio_4_3,
        VideoCropMode::default_crop);
    check(!aspect_4_3.crop_source, "aspect override does not crop source");
    check(
        rect_is(aspect_4_3.destination, 240, 0, 1440, 1080),
        "4:3 aspect override is exact and pillarboxed");

    const auto crop_4_3 = ps5mc::compute_video_layout(
        1920, 1080, 16.0 / 9.0, 1920, 1080,
        VideoScaleMode::fit,
        VideoAspectMode::default_ratio,
        VideoCropMode::ratio_4_3);
    check(crop_4_3.crop_source, "4:3 crop marks source crop");
    check(
        rect_is(crop_4_3.source, 240, 0, 1440, 1080),
        "4:3 crop removes source sides");
    check(
        rect_is(crop_4_3.destination, 240, 0, 1440, 1080),
        "4:3 crop remains undistorted on 16:9 output");

    const auto crop_scope = ps5mc::compute_video_layout(
        1920, 1080, 16.0 / 9.0, 1920, 1080,
        VideoScaleMode::fit,
        VideoAspectMode::default_ratio,
        VideoCropMode::ratio_2_35_1);
    check(
        rect_is(crop_scope.source, 0, 131, 1920, 817),
        "2.35:1 crop removes top and bottom");
    check(
        rect_is(crop_scope.destination, 0, 131, 1920, 817),
        "2.35:1 crop remains undistorted");

    const auto crop_then_aspect = ps5mc::compute_video_layout(
        1920, 1080, 16.0 / 9.0, 1920, 1080,
        VideoScaleMode::fit,
        VideoAspectMode::ratio_16_9,
        VideoCropMode::ratio_4_3);
    check(
        rect_is(crop_then_aspect.source, 240, 0, 1440, 1080),
        "crop is applied before aspect override");
    check(
        rect_is(crop_then_aspect.destination, 0, 0, 1920, 1080),
        "aspect override stretches cropped picture to requested ratio");

    check(
        ps5mc::parse_video_aspect_mode("2.39:1") ==
            VideoAspectMode::ratio_2_39_1,
        "parse VLC aspect ratio");
    check(
        std::string(ps5mc::video_aspect_mode_name(
            VideoAspectMode::default_ratio)) == "Original",
        "automatic aspect mode is clearly labeled Original");
    check(
        ps5mc::next_video_aspect_mode(VideoAspectMode::ratio_5_4) ==
            VideoAspectMode::default_ratio,
        "aspect list wraps");
    check(
        ps5mc::step_video_aspect_mode(
            VideoAspectMode::default_ratio,
            -1) == VideoAspectMode::ratio_5_4,
        "aspect list steps backward and wraps");
    check(
        ps5mc::parse_video_crop_mode("1.85:1") ==
            VideoCropMode::ratio_1_85_1,
        "parse VLC crop ratio");
    check(
        ps5mc::next_video_crop_mode(VideoCropMode::ratio_1_1) ==
            VideoCropMode::default_crop,
        "crop list wraps");
    check(
        ps5mc::step_video_crop_mode(
            VideoCropMode::default_crop,
            -1) == VideoCropMode::ratio_1_1,
        "crop list steps backward and wraps");

    const std::array<std::pair<VideoAspectMode, double>, 8> aspect_modes{{
        {VideoAspectMode::ratio_1_1, 1.0},
        {VideoAspectMode::ratio_4_3, 4.0 / 3.0},
        {VideoAspectMode::ratio_16_9, 16.0 / 9.0},
        {VideoAspectMode::ratio_16_10, 16.0 / 10.0},
        {VideoAspectMode::ratio_2_21_1, 2.21},
        {VideoAspectMode::ratio_2_35_1, 2.35},
        {VideoAspectMode::ratio_2_39_1, 2.39},
        {VideoAspectMode::ratio_5_4, 5.0 / 4.0},
    }};
    for (const auto& [mode, ratio] : aspect_modes) {
        const auto layout = ps5mc::compute_video_layout(
            1920, 1080, 16.0 / 9.0, 1920, 1080,
            VideoScaleMode::fit, mode, VideoCropMode::default_crop);
        const double actual =
            static_cast<double>(layout.destination.width) /
            static_cast<double>(layout.destination.height);
        check(
            std::abs(actual - ratio) < 0.005,
            "every VLC aspect override produces its exact display ratio");
        check(
            rect_is(layout.source, 0, 0, 1920, 1080),
            "aspect override keeps the complete source");
    }

    const std::array<std::pair<VideoCropMode, double>, 10> crop_modes{{
        {VideoCropMode::ratio_16_10, 16.0 / 10.0},
        {VideoCropMode::ratio_16_9, 16.0 / 9.0},
        {VideoCropMode::ratio_1_85_1, 1.85},
        {VideoCropMode::ratio_2_21_1, 2.21},
        {VideoCropMode::ratio_2_35_1, 2.35},
        {VideoCropMode::ratio_2_39_1, 2.39},
        {VideoCropMode::ratio_5_3, 5.0 / 3.0},
        {VideoCropMode::ratio_4_3, 4.0 / 3.0},
        {VideoCropMode::ratio_5_4, 5.0 / 4.0},
        {VideoCropMode::ratio_1_1, 1.0},
    }};
    for (const auto& [mode, ratio] : crop_modes) {
        const auto layout = ps5mc::compute_video_layout(
            1920, 1080, 16.0 / 9.0, 1920, 1080,
            VideoScaleMode::fit, VideoAspectMode::default_ratio, mode);
        const double source_ratio =
            static_cast<double>(layout.source.width) /
            static_cast<double>(layout.source.height);
        const double destination_ratio =
            static_cast<double>(layout.destination.width) /
            static_cast<double>(layout.destination.height);
        check(
            std::abs(source_ratio - ratio) < 0.005,
            "every VLC crop mode produces its exact source ratio");
        check(
            std::abs(destination_ratio - ratio) < 0.005,
            "every VLC crop mode stays undistorted at output");
    }

    if (failures == 0) {
        std::cout << "video_layout_tests: PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
