#include "bfplayer/list_navigation.hpp"

#include <cstdlib>

namespace bfplayer {

int wrap_list_index(
    int current,
    int delta,
    int item_count) noexcept {
    if (item_count <= 0) {
        return 0;
    }
    const long long count = item_count;
    long long value =
        (static_cast<long long>(current) + delta) % count;
    if (value < 0) {
        value += count;
    }
    return static_cast<int>(value);
}

void ListNavigationRepeat::press(
    int direction,
    std::uint64_t now_ms) noexcept {
    direction = direction < 0 ? -1 : (direction > 0 ? 1 : 0);
    if (direction == 0 || direction_ == direction) {
        return;
    }
    direction_ = direction;
    pressed_at_ms_ = now_ms;
    next_repeat_ms_ = now_ms + 350;
}

void ListNavigationRepeat::release(int direction) noexcept {
    direction = direction < 0 ? -1 : (direction > 0 ? 1 : 0);
    if (direction_ == direction) {
        reset();
    }
}

void ListNavigationRepeat::reset() noexcept {
    direction_ = 0;
    pressed_at_ms_ = 0;
    next_repeat_ms_ = 0;
}

int ListNavigationRepeat::poll(std::uint64_t now_ms) noexcept {
    if (direction_ == 0 || now_ms < next_repeat_ms_) {
        return 0;
    }
    const std::uint64_t held_ms = now_ms - pressed_at_ms_;
    int magnitude = 1;
    std::uint64_t interval_ms = 110;
    if (held_ms >= 2500) {
        magnitude = 5;
        interval_ms = 35;
    } else if (held_ms >= 1200) {
        magnitude = 2;
        interval_ms = 60;
    }
    next_repeat_ms_ = now_ms + interval_ms;
    return direction_ * magnitude;
}

} // namespace bfplayer
