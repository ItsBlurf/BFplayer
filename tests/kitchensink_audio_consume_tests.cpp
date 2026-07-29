#include "bfplayer/kitchensink_audio_consume.h"

#include <cstdlib>
#include <iostream>

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

    std::cout << "kitchensink_audio_consume_tests: PASS\n";
    return 0;
}
