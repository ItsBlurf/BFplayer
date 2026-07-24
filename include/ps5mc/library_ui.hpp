#pragma once

#include "ps5mc/media_sources.hpp"

#include <SDL_events.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_Renderer;

namespace ps5mc {

enum class LibraryAction {
    none,
    play,
    play_queue,
    exit,
};

class LibraryUi {
public:
    LibraryUi();
    ~LibraryUi();

    LibraryUi(const LibraryUi&) = delete;
    LibraryUi& operator=(const LibraryUi&) = delete;

    bool open(
        SDL_Renderer* renderer,
        const std::string& font_path,
        const std::string& logo_path,
        const std::vector<MediaSource>& initial_sources = {});
    void close();
    LibraryAction handle_event(const SDL_Event& event, std::string& selected_path);
    [[nodiscard]] std::vector<std::string> playback_queue(
        const std::string& selected_path) const;
    void show_notice(std::string message, std::uint64_t milliseconds = 5000);
    void render();

    [[nodiscard]] const std::string& error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ps5mc
