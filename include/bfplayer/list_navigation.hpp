#pragma once

#include <cstdint>

namespace bfplayer {

[[nodiscard]] int wrap_list_index(
    int current,
    int delta,
    int item_count) noexcept;

class ListNavigationRepeat {
public:
    void press(int direction, std::uint64_t now_ms) noexcept;
    void release(int direction) noexcept;
    void reset() noexcept;

    // Returns a signed list step, or zero when no repeat is due.
    [[nodiscard]] int poll(std::uint64_t now_ms) noexcept;

private:
    int direction_ = 0;
    std::uint64_t pressed_at_ms_ = 0;
    std::uint64_t next_repeat_ms_ = 0;
};

} // namespace bfplayer
