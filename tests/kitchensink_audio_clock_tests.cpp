#include "bfplayer/kitchensink_audio_clock.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool near(double left, double right) {
    return std::abs(left - right) < 0.000001;
}

} // namespace

int main() {
    constexpr int kRate = 48000;
    constexpr int kChannels = 2;
    constexpr int kBytes = 2;
    constexpr std::size_t kOneSecond = kRate * kChannels * kBytes;

    check(
        near(
            bfplayer_audio_audible_position(
                12.0, kOneSecond / 4, kRate, kChannels, kBytes),
            11.75),
        "queued audio is subtracted from the next sample timestamp");
    check(
        near(
            bfplayer_audio_audible_position(
                0.1, kOneSecond, kRate, kChannels, kBytes),
            0.0),
        "the audible clock is clamped to the beginning");
    check(
        near(
            bfplayer_audio_audible_position(
                42.5, 0, kRate, kChannels, kBytes),
            42.5),
        "an empty backend queue leaves the timestamp unchanged");
    check(
        std::isnan(
            bfplayer_audio_audible_position(
                1.0, 0, 0, kChannels, kBytes)),
        "invalid audio formats are rejected");
    check(
        std::isnan(
            bfplayer_audio_audible_position(
                NAN, 0, kRate, kChannels, kBytes)),
        "invalid timestamps are rejected");
    check(
        std::isnan(
            bfplayer_audio_audible_position(
                20.0, kOneSecond * 11, kRate, kChannels, kBytes)),
        "sentinel or implausibly large backend queues are rejected");
    check(
        near(bfplayer_audio_monotonic_position(18.5, NAN), 18.5),
        "the first timestamp establishes the monotonic clock");
    check(
        near(bfplayer_audio_monotonic_position(18.75, 18.5), 18.75),
        "forward timestamps advance the monotonic clock");
    check(
        near(bfplayer_audio_monotonic_position(0.25, 18.75), 18.75),
        "an EOF timestamp reset cannot rewind playback");
    check(
        std::isnan(bfplayer_audio_monotonic_position(NAN, 18.75)),
        "invalid candidate timestamps remain invalid");

    std::cout << "kitchensink_audio_clock_tests: PASS\n";
    return 0;
}
