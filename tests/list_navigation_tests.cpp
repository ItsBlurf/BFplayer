#include "bfplayer/list_navigation.hpp"

#include <iostream>

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
    check(
        bfplayer::wrap_list_index(0, -1, 5) == 4,
        "up from first wraps to last");
    check(
        bfplayer::wrap_list_index(4, 1, 5) == 0,
        "down from last wraps to first");
    check(
        bfplayer::wrap_list_index(1, 12, 5) == 3,
        "large accelerated steps wrap");
    check(
        bfplayer::wrap_list_index(7, 1, 0) == 0,
        "empty lists remain at zero");

    bfplayer::ListNavigationRepeat repeat;
    repeat.press(1, 1000);
    check(repeat.poll(1349) == 0, "hold delay prevents accidental repeat");
    check(repeat.poll(1350) == 1, "hold begins with single steps");
    check(repeat.poll(1459) == 0, "initial repeat interval is bounded");
    check(repeat.poll(2200) == 2, "long hold accelerates");
    check(repeat.poll(3500) == 5, "very long hold accelerates further");
    repeat.release(1);
    check(repeat.poll(10000) == 0, "release stops repeating");

    if (failures == 0) {
        std::cout << "list_navigation_tests: PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
