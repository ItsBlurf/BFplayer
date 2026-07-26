#include "ps5mc/playback_osd.hpp"

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ps5mc {
namespace {

constexpr int kScreenWidth = 1920;
constexpr int kScreenHeight = 1080;

std::string format_time(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        seconds = 0.0;
    }
    const double bounded = std::min(
        seconds,
        static_cast<double>(std::numeric_limits<int>::max()));
    const int total = static_cast<int>(std::llround(bounded));
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int remainder = total % 60;
    char buffer[32]{};
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours, minutes, remainder);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, remainder);
    }
    return buffer;
}

} // namespace

struct PlaybackOsd::Impl {
    SDL_Renderer* renderer = nullptr;
    TTF_Font* font = nullptr;
    SDL_Texture* message_texture = nullptr;
    SDL_Texture* time_texture = nullptr;
    SDL_Texture* panel_title_texture = nullptr;
    std::vector<SDL_Texture*> panel_row_textures;
    std::vector<int> panel_row_widths;
    std::vector<int> panel_row_heights;
    std::string message;
    std::string rendered_message;
    std::string rendered_time;
    std::string last_error;
    int message_width = 0;
    int message_height = 0;
    int time_width = 0;
    int time_height = 0;
    std::uint64_t visible_until = 0;
    std::uint64_t default_duration = 4000;
    bool panel_visible = false;
    int panel_selected = 0;
    int panel_title_width = 0;
    int panel_title_height = 0;

    void destroy_textures() {
        SDL_DestroyTexture(message_texture);
        SDL_DestroyTexture(time_texture);
        message_texture = nullptr;
        time_texture = nullptr;
        rendered_message.clear();
        rendered_time.clear();
    }

    void destroy_panel() {
        SDL_DestroyTexture(panel_title_texture);
        panel_title_texture = nullptr;
        for (SDL_Texture* texture : panel_row_textures) {
            SDL_DestroyTexture(texture);
        }
        panel_row_textures.clear();
        panel_row_widths.clear();
        panel_row_heights.clear();
        panel_title_width = 0;
        panel_title_height = 0;
        panel_selected = 0;
        panel_visible = false;
    }

    bool update_texture(
        const std::string& text,
        std::string& rendered,
        SDL_Texture*& texture,
        int& width,
        int& height,
        SDL_Color color) {
        if (text == rendered && texture) {
            return true;
        }
        SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
        if (!surface) {
            last_error = std::string("TTF_RenderUTF8_Blended: ") + TTF_GetError();
            return false;
        }
        SDL_Texture* replacement = SDL_CreateTextureFromSurface(renderer, surface);
        width = surface->w;
        height = surface->h;
        SDL_FreeSurface(surface);
        if (!replacement) {
            last_error = std::string("SDL_CreateTextureFromSurface: ") + SDL_GetError();
            return false;
        }
        SDL_DestroyTexture(texture);
        texture = replacement;
        rendered = text;
        return true;
    }
};

PlaybackOsd::PlaybackOsd() : impl_(std::make_unique<Impl>()) {}

PlaybackOsd::~PlaybackOsd() {
    close();
}

bool PlaybackOsd::open(SDL_Renderer* renderer, const std::string& font_path) {
    close();
    impl_->last_error.clear();
    impl_->renderer = renderer;
    if (!renderer || font_path.empty()) {
        impl_->last_error = "PlaybackOsd::open: invalid argument";
        return false;
    }
    if (TTF_Init() != 0) {
        impl_->last_error = std::string("TTF_Init: ") + TTF_GetError();
        return false;
    }
    impl_->font = TTF_OpenFont(font_path.c_str(), 31);
    if (!impl_->font) {
        impl_->last_error = std::string("TTF_OpenFont: ") + TTF_GetError();
        TTF_Quit();
        return false;
    }
    return true;
}

void PlaybackOsd::close() {
    if (!impl_) {
        return;
    }
    impl_->destroy_textures();
    impl_->destroy_panel();
    if (impl_->font) {
        TTF_CloseFont(impl_->font);
        impl_->font = nullptr;
    }
    if (TTF_WasInit()) {
        TTF_Quit();
    }
    impl_->renderer = nullptr;
    impl_->message.clear();
    impl_->visible_until = 0;
}

void PlaybackOsd::set_default_duration(std::uint64_t milliseconds) {
    impl_->default_duration = std::clamp<std::uint64_t>(
        milliseconds,
        500,
        30000);
}

void PlaybackOsd::show(std::string message, std::uint64_t milliseconds) {
    constexpr std::size_t kMaximumMessageBytes = 512;
    if (message.size() > kMaximumMessageBytes) {
        message.resize(kMaximumMessageBytes);
        while (!message.empty() &&
               (static_cast<unsigned char>(message.back()) & 0xc0U) == 0x80U) {
            message.pop_back();
        }
        if (!message.empty() &&
            static_cast<unsigned char>(message.back()) >= 0x80U) {
            message.pop_back();
        }
        message += "...";
    }
    impl_->message = std::move(message);
    if (milliseconds == 0) {
        milliseconds = impl_->default_duration;
    }
    const std::uint64_t now = SDL_GetTicks64();
    impl_->visible_until = now > UINT64_MAX - milliseconds ? UINT64_MAX : now + milliseconds;
}

void PlaybackOsd::show_panel(
    std::string title,
    std::vector<std::string> rows,
    int selected) {
    impl_->destroy_panel();
    if (!impl_->renderer || !impl_->font) {
        return;
    }
    const SDL_Color white{244, 247, 255, 255};
    auto make_texture = [&](const std::string& text, int& width, int& height) {
        SDL_Surface* surface =
            TTF_RenderUTF8_Blended(impl_->font, text.c_str(), white);
        if (!surface) {
            return static_cast<SDL_Texture*>(nullptr);
        }
        SDL_Texture* texture =
            SDL_CreateTextureFromSurface(impl_->renderer, surface);
        width = surface->w;
        height = surface->h;
        SDL_FreeSurface(surface);
        return texture;
    };
    impl_->panel_title_texture = make_texture(
        title,
        impl_->panel_title_width,
        impl_->panel_title_height);
    impl_->panel_row_textures.reserve(rows.size());
    impl_->panel_row_widths.reserve(rows.size());
    impl_->panel_row_heights.reserve(rows.size());
    for (const std::string& row : rows) {
        int width = 0;
        int height = 0;
        impl_->panel_row_textures.push_back(
            make_texture(row, width, height));
        impl_->panel_row_widths.push_back(width);
        impl_->panel_row_heights.push_back(height);
    }
    impl_->panel_selected = rows.empty() || selected < 0
        ? -1
        : std::clamp(selected, 0, static_cast<int>(rows.size()) - 1);
    impl_->panel_visible = true;
}

void PlaybackOsd::hide_panel() {
    impl_->destroy_panel();
}

bool PlaybackOsd::panel_visible() const noexcept {
    return impl_->panel_visible;
}

void PlaybackOsd::render(double position_seconds, double duration_seconds, bool paused) {
    if (!impl_->renderer || !impl_->font) {
        return;
    }
    const std::uint64_t now = SDL_GetTicks64();
    if (!paused && !impl_->panel_visible && now >= impl_->visible_until) {
        return;
    }
    SDL_RenderSetLogicalSize(impl_->renderer, kScreenWidth, kScreenHeight);
    SDL_SetRenderDrawBlendMode(impl_->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(impl_->renderer, 5, 8, 15, 220);
    SDL_Rect panel{0, kScreenHeight - 170, kScreenWidth, 170};
    SDL_RenderFillRect(impl_->renderer, &panel);

    const int bar_x = 58;
    const int bar_y = kScreenHeight - 66;
    const int bar_width = kScreenWidth - 116;
    SDL_SetRenderDrawColor(impl_->renderer, 74, 83, 101, 255);
    SDL_Rect bar{bar_x, bar_y, bar_width, 10};
    SDL_RenderFillRect(impl_->renderer, &bar);
    const double ratio =
        std::isfinite(position_seconds) &&
            std::isfinite(duration_seconds) &&
            duration_seconds > 0.0
        ? std::clamp(position_seconds / duration_seconds, 0.0, 1.0)
        : 0.0;
    SDL_SetRenderDrawColor(impl_->renderer, 66, 139, 255, 255);
    SDL_Rect progress{bar_x, bar_y, static_cast<int>(std::llround(bar_width * ratio)), 10};
    SDL_RenderFillRect(impl_->renderer, &progress);

    const std::string time =
        format_time(position_seconds) + " / " + format_time(duration_seconds);
    const SDL_Color white{244, 247, 255, 255};
    const SDL_Color muted{185, 196, 218, 255};
    impl_->update_texture(
        paused ? "Paused - " + impl_->message : impl_->message,
        impl_->rendered_message,
        impl_->message_texture,
        impl_->message_width,
        impl_->message_height,
        white);
    impl_->update_texture(
        time,
        impl_->rendered_time,
        impl_->time_texture,
        impl_->time_width,
        impl_->time_height,
        muted);
    if (impl_->message_texture) {
        SDL_Rect target{58, kScreenHeight - 145, impl_->message_width, impl_->message_height};
        SDL_RenderCopy(impl_->renderer, impl_->message_texture, nullptr, &target);
    }
    if (impl_->time_texture) {
        SDL_Rect target{
            kScreenWidth - impl_->time_width - 58,
            kScreenHeight - 145,
            impl_->time_width,
            impl_->time_height};
        SDL_RenderCopy(impl_->renderer, impl_->time_texture, nullptr, &target);
    }

    if (impl_->panel_visible) {
        constexpr int panel_width = 1320;
        constexpr int panel_x = (kScreenWidth - panel_width) / 2;
        constexpr int panel_y = 126;
        constexpr int row_height = 52;
        const int row_count =
            static_cast<int>(impl_->panel_row_textures.size());
        const int panel_height =
            std::min(820, 112 + row_count * row_height);
        SDL_SetRenderDrawColor(impl_->renderer, 7, 12, 23, 248);
        SDL_Rect modal{panel_x, panel_y, panel_width, panel_height};
        SDL_RenderFillRect(impl_->renderer, &modal);
        SDL_SetRenderDrawColor(impl_->renderer, 58, 133, 231, 255);
        SDL_RenderDrawRect(impl_->renderer, &modal);
        if (impl_->panel_title_texture) {
            SDL_Rect target{
                panel_x + 42,
                panel_y + 28,
                impl_->panel_title_width,
                impl_->panel_title_height};
            SDL_RenderCopy(
                impl_->renderer,
                impl_->panel_title_texture,
                nullptr,
                &target);
        }
        for (int index = 0; index < row_count; ++index) {
            SDL_Rect background{
                panel_x + 28,
                panel_y + 88 + index * row_height,
                panel_width - 56,
                row_height - 6};
            if (index == impl_->panel_selected) {
                SDL_SetRenderDrawColor(impl_->renderer, 25, 67, 122, 255);
                SDL_RenderFillRect(impl_->renderer, &background);
                SDL_SetRenderDrawColor(impl_->renderer, 83, 164, 255, 255);
                SDL_Rect marker{
                    background.x,
                    background.y,
                    6,
                    background.h};
                SDL_RenderFillRect(impl_->renderer, &marker);
            }
            SDL_Texture* texture =
                impl_->panel_row_textures[static_cast<std::size_t>(index)];
            if (texture) {
                SDL_Rect target{
                    background.x + 22,
                    background.y +
                        (background.h -
                         impl_->panel_row_heights[static_cast<std::size_t>(index)]) /
                            2,
                    impl_->panel_row_widths[static_cast<std::size_t>(index)],
                    impl_->panel_row_heights[static_cast<std::size_t>(index)]};
                SDL_RenderSetClipRect(impl_->renderer, &background);
                SDL_RenderCopy(impl_->renderer, texture, nullptr, &target);
                SDL_RenderSetClipRect(impl_->renderer, nullptr);
            }
        }
    }
}

const std::string& PlaybackOsd::error() const noexcept {
    return impl_->last_error;
}

} // namespace ps5mc
