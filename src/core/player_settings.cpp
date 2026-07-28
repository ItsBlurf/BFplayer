#include "bfplayer/player_settings.hpp"
#include "bfplayer/subtitle_provider.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>

namespace bfplayer {
namespace {

template <std::size_t Size>
int cycle_choice(
    const std::array<int, Size>& choices,
    int current,
    int direction) noexcept {
    const auto found = std::find(choices.begin(), choices.end(), current);
    std::size_t index = found == choices.end()
        ? 0U
        : static_cast<std::size_t>(found - choices.begin());
    if (direction < 0) {
        index = (index + Size - 1U) % Size;
    } else {
        index = (index + 1U) % Size;
    }
    return choices[index];
}

} // namespace

PlayerSettings normalized_player_settings(PlayerSettings settings) noexcept {
    settings.volume_percent = std::clamp(settings.volume_percent, 0, 100);
    static constexpr std::array<int, 4> short_choices{5, 10, 15, 30};
    static constexpr std::array<int, 4> long_choices{30, 60, 120, 300};
    static constexpr std::array<int, 4> osd_choices{2000, 4000, 6000, 8000};
    if (std::find(
            short_choices.begin(),
            short_choices.end(),
            settings.short_seek_seconds) == short_choices.end()) {
        settings.short_seek_seconds = 10;
    }
    if (std::find(
            long_choices.begin(),
            long_choices.end(),
            settings.long_seek_seconds) == long_choices.end()) {
        settings.long_seek_seconds = 60;
    }
    if (std::find(
            osd_choices.begin(),
            osd_choices.end(),
            settings.osd_duration_ms) == osd_choices.end()) {
        settings.osd_duration_ms = 4000;
    }
    settings.subdl_api_key.erase(
        std::remove_if(
            settings.subdl_api_key.begin(),
            settings.subdl_api_key.end(),
            [](unsigned char character) {
                return character < 0x21U || character > 0x7eU ||
                       character == '\r' || character == '\n';
            }),
        settings.subdl_api_key.end());
    if (settings.subdl_api_key.size() > 256) {
        settings.subdl_api_key.resize(256);
    }
    settings.subtitle_languages =
        normalize_subtitle_languages(
            std::move(settings.subtitle_languages));
    return settings;
}

bool parse_setting_integer(
    const std::string& text,
    int minimum,
    int maximum,
    int& output) noexcept {
    if (text.empty() || minimum > maximum) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        return false;
    }
    output = static_cast<int>(parsed);
    return true;
}

bool parse_setting_boolean(const std::string& text, bool& output) noexcept {
    if (text == "1" || text == "true" || text == "on") {
        output = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "off") {
        output = false;
        return true;
    }
    return false;
}

int next_short_seek_seconds(int current, int direction) noexcept {
    return cycle_choice(
        std::array<int, 4>{5, 10, 15, 30},
        current,
        direction);
}

int next_long_seek_seconds(int current, int direction) noexcept {
    return cycle_choice(
        std::array<int, 4>{30, 60, 120, 300},
        current,
        direction);
}

int next_osd_duration_ms(int current, int direction) noexcept {
    return cycle_choice(
        std::array<int, 4>{2000, 4000, 6000, 8000},
        current,
        direction);
}

} // namespace bfplayer
