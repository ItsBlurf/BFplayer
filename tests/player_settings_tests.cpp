#include "bfplayer/player_settings.hpp"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    bfplayer::PlayerSettings invalid{};
    invalid.volume_percent = 140;
    invalid.short_seek_seconds = 7;
    invalid.long_seek_seconds = -1;
    invalid.osd_duration_ms = 17;
    invalid.subdl_api_key = " key\r\nvalue ";
    invalid.subtitle_languages = " EN; AR,fa ";
    const bfplayer::PlayerSettings normalized =
        bfplayer::normalized_player_settings(invalid);
    check(normalized.volume_percent == 100, "volume is clamped");
    check(normalized.short_seek_seconds == 10, "invalid short seek resets");
    check(normalized.long_seek_seconds == 60, "invalid long seek resets");
    check(normalized.osd_duration_ms == 4000, "invalid OSD duration resets");
    check(
        normalized.subdl_api_key == "keyvalue",
        "provider API key removes unsafe whitespace");
    check(
        normalized.subtitle_languages == "en,ar,fa",
        "subtitle languages normalize");

    int integer = 0;
    check(bfplayer::parse_setting_integer("30", 0, 100, integer) &&
              integer == 30,
          "valid integer parses");
    check(!bfplayer::parse_setting_integer("30x", 0, 100, integer),
          "trailing integer text is rejected");
    check(!bfplayer::parse_setting_integer("101", 0, 100, integer),
          "out-of-range integer is rejected");

    bool boolean = false;
    check(bfplayer::parse_setting_boolean("on", boolean) && boolean,
          "on parses true");
    check(bfplayer::parse_setting_boolean("0", boolean) && !boolean,
          "zero parses false");
    check(!bfplayer::parse_setting_boolean("maybe", boolean),
          "unknown boolean is rejected");

    check(bfplayer::next_short_seek_seconds(10, 1) == 15,
          "short seek advances");
    check(bfplayer::next_short_seek_seconds(5, -1) == 30,
          "short seek wraps backward");
    check(bfplayer::next_long_seek_seconds(300, 1) == 30,
          "long seek wraps");
    check(bfplayer::next_osd_duration_ms(4000, -1) == 2000,
          "OSD duration moves backward");

    if (failures == 0) {
        std::cout << "player_settings_tests: PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
