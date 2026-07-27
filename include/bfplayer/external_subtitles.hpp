#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct SDL_Renderer;

namespace bfplayer {

// External text-subtitle renderer. FFmpeg demuxes/decodes the sidecar and
// converts supported text formats to ASS events; libass handles layout and
// styling. FFmpeg bitmap cues (SUP/PGS, DVD IDX+SUB, DVB) use an independent
// palette-to-RGBA path.
class ExternalSubtitles {
public:
    ExternalSubtitles();
    ~ExternalSubtitles();

    ExternalSubtitles(const ExternalSubtitles&) = delete;
    ExternalSubtitles& operator=(const ExternalSubtitles&) = delete;

    bool open(
        SDL_Renderer* renderer,
        const std::string& path,
        const std::string& fallback_font,
        int screen_width,
        int screen_height);
    void close();
    bool resize(int screen_width, int screen_height);

    // Draws at the current movie clock plus a signed user delay. Returns false
    // only for a rendering error; an empty subtitle frame is successful.
    bool render(std::int64_t movie_position_ms, std::int64_t delay_ms = 0);

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bfplayer
