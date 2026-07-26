#pragma once

#include <string>
#include <string_view>

namespace ps5mc {

inline constexpr std::string_view kSettingVolumePercent =
    "volume_percent";
inline constexpr std::string_view kSettingShortSeekSeconds =
    "playback.short_seek_seconds";
inline constexpr std::string_view kSettingLongSeekSeconds =
    "playback.long_seek_seconds";
inline constexpr std::string_view kSettingOsdDurationMs =
    "playback.osd_duration_ms";
inline constexpr std::string_view kSettingResumePlayback =
    "playback.resume_enabled";
inline constexpr std::string_view kSettingAutoSubtitles =
    "playback.auto_subtitles";

struct PlayerSettings {
    int volume_percent = 100;
    int short_seek_seconds = 10;
    int long_seek_seconds = 60;
    int osd_duration_ms = 4000;
    bool resume_playback = true;
    bool auto_subtitles = true;
};

[[nodiscard]] PlayerSettings normalized_player_settings(
    PlayerSettings settings) noexcept;
[[nodiscard]] bool parse_setting_integer(
    const std::string& text,
    int minimum,
    int maximum,
    int& output) noexcept;
[[nodiscard]] bool parse_setting_boolean(
    const std::string& text,
    bool& output) noexcept;
[[nodiscard]] int next_short_seek_seconds(int current, int direction) noexcept;
[[nodiscard]] int next_long_seek_seconds(int current, int direction) noexcept;
[[nodiscard]] int next_osd_duration_ms(int current, int direction) noexcept;

} // namespace ps5mc
