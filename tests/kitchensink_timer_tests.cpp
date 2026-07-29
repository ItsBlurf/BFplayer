#include "kitchensink2/internal/kittimerstate.h"

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
    Kit_TimerState state{};

    check(
        near(Kit_TimerStateElapsed(&state, 100.0), 0.0),
        "an uninitialized clock starts at zero");
    Kit_TimerStateInit(&state, 100.0);
    check(
        near(Kit_TimerStateElapsed(&state, 103.5), 3.5),
        "a playing clock follows monotonic time");

    Kit_TimerStatePause(&state, 103.5);
    check(
        near(Kit_TimerStateElapsed(&state, 500.0), 3.5),
        "a paused clock remains frozen");
    Kit_TimerStateResume(&state, 500.0);
    check(
        near(Kit_TimerStateElapsed(&state, 502.0), 5.5),
        "resume continues from the frozen position");

    Kit_TimerStatePause(&state, 502.0);
    Kit_TimerStateAdjust(&state, 502.0, 42.0);
    check(
        near(Kit_TimerStateElapsed(&state, 900.0), 42.0),
        "seeking while paused changes the frozen position");
    Kit_TimerStateResume(&state, 900.0);
    check(
        near(Kit_TimerStateElapsed(&state, 901.25), 43.25),
        "resume after a paused seek starts from the seek target");

    Kit_TimerStateReset(&state);
    check(
        near(Kit_TimerStateElapsed(&state, 1000.0), 0.0),
        "reset clears the prior timeline");

    std::cout << "kitchensink_timer_tests: PASS\n";
    return 0;
}
