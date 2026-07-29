#include "kitchensink2/internal/kitpacketbudget.h"

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
    static_assert(
        KIT_VIDEO_PACKET_BYTE_LIMIT == 96U * 1024U * 1024U);
    static_assert(
        KIT_AUDIO_PACKET_BYTE_LIMIT == 16U * 1024U * 1024U);
    static_assert(
        KIT_SUBTITLE_PACKET_BYTE_LIMIT == 8U * 1024U * 1024U);
    check(
        Kit_PacketBudgetAllows(1024, 0, 4096, false),
        "zero limit leaves upstream behavior unchanged");
    check(
        Kit_PacketBudgetAllows(0, 1024, 2048, true),
        "one oversized packet is allowed into an empty queue");
    check(
        Kit_PacketBudgetAllows(512, 1024, 512, false),
        "packet fitting the remaining budget is allowed");
    check(
        !Kit_PacketBudgetAllows(513, 1024, 512, false),
        "packet exceeding the remaining budget is blocked");
    check(
        !Kit_PacketBudgetAllows(1024, 1024, 0, false),
        "a full byte budget blocks another queued packet");
    check(
        Kit_PacketBudgetAdd(
            std::numeric_limits<std::size_t>::max() - 2,
            4) ==
            std::numeric_limits<std::size_t>::max(),
        "byte accounting saturates on overflow");
    check(
        Kit_PacketBudgetRemove(128, 512) == 0,
        "byte accounting cannot underflow");

    std::cout << "packet_budget_tests: PASS\n";
    return 0;
}
