#include "ps5mc/player_settings.hpp"

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
    ps5mc::PlayerSettings invalid{};
    invalid.volume_percent = 140;
    invalid.short_seek_seconds = 7;
    invalid.long_seek_seconds = -1;
    invalid.osd_duration_ms = 17;
    const ps5mc::PlayerSettings normalized =
        ps5mc::normalized_player_settings(invalid);
    check(normalized.volume_percent == 100, "volume is clamped");
    check(normalized.short_seek_seconds == 10, "invalid short seek resets");
    check(normalized.long_seek_seconds == 60, "invalid long seek resets");
    check(normalized.osd_duration_ms == 4000, "invalid OSD duration resets");

    int integer = 0;
    check(ps5mc::parse_setting_integer("30", 0, 100, integer) &&
              integer == 30,
          "valid integer parses");
    check(!ps5mc::parse_setting_integer("30x", 0, 100, integer),
          "trailing integer text is rejected");
    check(!ps5mc::parse_setting_integer("101", 0, 100, integer),
          "out-of-range integer is rejected");

    bool boolean = false;
    check(ps5mc::parse_setting_boolean("on", boolean) && boolean,
          "on parses true");
    check(ps5mc::parse_setting_boolean("0", boolean) && !boolean,
          "zero parses false");
    check(!ps5mc::parse_setting_boolean("maybe", boolean),
          "unknown boolean is rejected");

    check(ps5mc::next_short_seek_seconds(10, 1) == 15,
          "short seek advances");
    check(ps5mc::next_short_seek_seconds(5, -1) == 30,
          "short seek wraps backward");
    check(ps5mc::next_long_seek_seconds(300, 1) == 30,
          "long seek wraps");
    check(ps5mc::next_osd_duration_ms(4000, -1) == 2000,
          "OSD duration moves backward");

    if (failures == 0) {
        std::cout << "player_settings_tests: PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
