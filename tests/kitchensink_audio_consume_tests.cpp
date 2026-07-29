#include "bfplayer/kitchensink_audio_consume.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    BfplayerAudioConsumePlan plan{};
    BfplayerAudioSeekPlan seek{};

    check(
        bfplayer_audio_consume_plan(131072, 131072, 65536, 4, &plan) &&
            plan.offset == 0 &&
            plan.bytes == 65536 &&
            plan.remaining == 65536,
        "the first partial read preserves the unconsumed half");
    check(
        bfplayer_audio_consume_plan(
            131072, plan.remaining, 65536, 4, &plan) &&
            plan.offset == 65536 &&
            plan.bytes == 65536 &&
            plan.remaining == 0,
        "the second read resumes at the preserved offset");
    check(
        bfplayer_audio_consume_plan(4096, 4096, 1023, 4, &plan) &&
            plan.bytes == 1020 &&
            plan.remaining == 3076,
        "backend capacity is aligned to complete sample frames");
    check(
        !bfplayer_audio_consume_plan(4096, 4097, 1024, 4, &plan),
        "remaining bytes cannot exceed the decoded frame");
    check(
        !bfplayer_audio_consume_plan(4095, 4095, 1024, 4, &plan),
        "misaligned decoded frames are rejected");
    check(
        !bfplayer_audio_consume_plan(4096, 4096, 1024, 0, &plan),
        "zero-sized sample frames are rejected");
    check(
        bfplayer_audio_seek_plan(
            192000,
            192000,
            4,
            10.0,
            192000.0,
            10.25,
            &seek) &&
            seek.skip_bytes == 48000 &&
            seek.target_reached,
        "seek trimming lands on the requested sample");
    check(
        bfplayer_audio_seek_plan(
            48000,
            48000,
            4,
            7.0,
            192000.0,
            8.0,
            &seek) &&
            seek.skip_bytes == 48000 &&
            !seek.target_reached,
        "a fully stale decoded frame is discarded");
    check(
        bfplayer_audio_seek_plan(
            4096,
            2048,
            4,
            2.0,
            192000.0,
            2.005,
            &seek) &&
            seek.skip_bytes == 0 &&
            seek.target_reached,
        "the consumed portion contributes to the current timestamp");
    check(
        bfplayer_audio_seek_plan(
            4096,
            4096,
            4,
            3.0,
            192000.0,
            2.0,
            &seek) &&
            seek.skip_bytes == 0 &&
            seek.target_reached,
        "audio at or after the seek target is preserved");
    check(
        !bfplayer_audio_seek_plan(
            4095,
            4095,
            4,
            0.0,
            192000.0,
            1.0,
            &seek),
        "invalid seek frame accounting is rejected");
    const auto max_aligned =
        std::numeric_limits<std::size_t>::max() -
        std::numeric_limits<std::size_t>::max() % 8u;
    check(
        bfplayer_audio_seek_plan(
            max_aligned,
            max_aligned,
            8u,
            0.0,
            1.0e18,
            static_cast<double>(max_aligned) / 1.0e18,
            &seek) &&
            seek.skip_bytes <= max_aligned &&
            seek.skip_bytes % 8u == 0u,
        "seek trimming cannot overflow or exceed the remaining frame");

    std::cout << "kitchensink_audio_consume_tests: PASS\n";
    return 0;
}
