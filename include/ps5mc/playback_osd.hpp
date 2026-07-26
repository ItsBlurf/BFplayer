#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_Renderer;

namespace ps5mc {

class PlaybackOsd {
public:
    PlaybackOsd();
    ~PlaybackOsd();

    PlaybackOsd(const PlaybackOsd&) = delete;
    PlaybackOsd& operator=(const PlaybackOsd&) = delete;

    bool open(SDL_Renderer* renderer, const std::string& font_path);
    void close();
    void set_default_duration(std::uint64_t milliseconds);
    void show(std::string message, std::uint64_t milliseconds = 0);
    void show_panel(
        std::string title,
        std::vector<std::string> rows,
        int selected);
    void hide_panel();
    [[nodiscard]] bool panel_visible() const noexcept;
    void render(double position_seconds, double duration_seconds, bool paused);

    [[nodiscard]] const std::string& error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ps5mc
