#include "ps5mc/library_ui.hpp"

#include "ps5mc/artwork.hpp"
#include "ps5mc/bulk_import.hpp"
#include "ps5mc/controller_buttons.hpp"
#include "ps5mc/diagnostics.hpp"
#include "ps5mc/library_database.hpp"
#include "ps5mc/library_scanner.hpp"
#include "ps5mc/library_view.hpp"
#include "ps5mc/media_sources.hpp"
#include "ps5mc/media_probe.hpp"
#include "ps5mc/player_settings.hpp"
#include "ps5mc/video_thumbnail.hpp"

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unordered_set>
#include <utility>
#include <vector>
#include <unistd.h>

#ifndef PS5MC_VERSION
#define PS5MC_VERSION "development"
#endif

namespace ps5mc {
namespace {

constexpr int kUiWidth = 1920;
constexpr int kUiHeight = 1080;
constexpr int kVisibleRows = 11;
constexpr int kRowHeight = 66;
constexpr int kRowsTop = 226;
constexpr int kListWidth = 1228;
constexpr int kArtworkPanelX = 1310;
constexpr int kArtworkPanelWidth = 552;
constexpr int kArtworkPanelHeight = 720;
constexpr int kFilterCount = 8;
constexpr std::size_t kMetadataProbeBudget = 512;
constexpr std::size_t kMetadataFailureBudget = 8;
constexpr std::size_t kMaxSearchBytes = 512;
constexpr std::size_t kMaxBrowserEntriesSeen = 100000;
constexpr std::size_t kMaxBrowserItems = 50000;
constexpr std::size_t kMaxBrowserPathBytes = 4096;

struct Label {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
};

enum class FooterGlyph {
    cross,
    circle,
    square,
    triangle,
    dpad,
    touchpad,
    options,
    text_button,
};

struct FooterHint {
    FooterGlyph glyph = FooterGlyph::cross;
    Label button_label;
    Label action_label;
};

enum class LibraryOverlay {
    none,
    menu,
    settings,
    controls,
    about,
};

void destroy_label(Label& label) {
    SDL_DestroyTexture(label.texture);
    label = {};
}

Label make_label(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, SDL_Color color) {
    Label label{};
    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surface) {
        return label;
    }
    label.texture = SDL_CreateTextureFromSurface(renderer, surface);
    label.width = surface->w;
    label.height = surface->h;
    SDL_FreeSurface(surface);
    return label;
}

int footer_glyph_width(const FooterHint& hint) {
    if (hint.glyph == FooterGlyph::touchpad) {
        return 43;
    }
    if (hint.glyph == FooterGlyph::options) {
        return 36;
    }
    if (hint.glyph == FooterGlyph::text_button) {
        return std::max(38, hint.button_label.width + 14);
    }
    return 30;
}

void draw_footer_glyph(
    SDL_Renderer* renderer,
    const FooterHint& hint,
    int x,
    int center_y) {
    const int width = footer_glyph_width(hint);
    const int center_x = x + width / 2;
    SDL_SetRenderDrawColor(renderer, 226, 235, 251, 255);
    switch (hint.glyph) {
    case FooterGlyph::cross:
        SDL_RenderDrawLine(renderer, center_x - 10, center_y - 10,
                          center_x + 10, center_y + 10);
        SDL_RenderDrawLine(renderer, center_x + 10, center_y - 10,
                          center_x - 10, center_y + 10);
        break;
    case FooterGlyph::circle:
        for (int radius = 11; radius <= 12; ++radius) {
            for (int degree = 0; degree < 360; degree += 4) {
                const double angle = degree * 3.14159265358979323846 / 180.0;
                SDL_RenderDrawPoint(
                    renderer,
                    center_x + static_cast<int>(radius * std::cos(angle)),
                    center_y + static_cast<int>(radius * std::sin(angle)));
            }
        }
        break;
    case FooterGlyph::square: {
        SDL_Rect square{center_x - 11, center_y - 11, 23, 23};
        SDL_RenderDrawRect(renderer, &square);
        break;
    }
    case FooterGlyph::triangle:
        SDL_RenderDrawLine(renderer, center_x, center_y - 13,
                          center_x - 13, center_y + 11);
        SDL_RenderDrawLine(renderer, center_x - 13, center_y + 11,
                          center_x + 13, center_y + 11);
        SDL_RenderDrawLine(renderer, center_x + 13, center_y + 11,
                          center_x, center_y - 13);
        break;
    case FooterGlyph::dpad: {
        SDL_Rect vertical{center_x - 4, center_y - 14, 9, 29};
        SDL_Rect horizontal{center_x - 14, center_y - 4, 29, 9};
        SDL_RenderDrawRect(renderer, &vertical);
        SDL_RenderDrawRect(renderer, &horizontal);
        break;
    }
    case FooterGlyph::touchpad: {
        SDL_Rect touchpad{x, center_y - 12, width, 25};
        SDL_RenderDrawRect(renderer, &touchpad);
        SDL_RenderDrawLine(renderer, center_x, center_y - 9,
                          center_x, center_y + 9);
        break;
    }
    case FooterGlyph::options: {
        for (int offset = -7; offset <= 7; offset += 7) {
            SDL_RenderDrawLine(
                renderer, x + 4, center_y + offset,
                x + width - 4, center_y + offset);
        }
        break;
    }
    case FooterGlyph::text_button: {
        SDL_Rect button{x, center_y - 14, width, 29};
        SDL_RenderDrawRect(renderer, &button);
        if (hint.button_label.texture) {
            SDL_Rect target{
                x + (width - hint.button_label.width) / 2,
                center_y - hint.button_label.height / 2,
                hint.button_label.width,
                hint.button_label.height};
            SDL_RenderCopy(renderer, hint.button_label.texture, nullptr, &target);
        }
        break;
    }
    }
}

std::string fit_text_to_width(TTF_Font* font, std::string text, int max_width) {
    int width = 0;
    int height = 0;
    if (TTF_SizeUTF8(font, text.c_str(), &width, &height) == 0 && width <= max_width) {
        return text;
    }
    constexpr const char* suffix = "...";
    while (!text.empty()) {
        erase_last_utf8_codepoint(text);
        const std::string candidate = text + suffix;
        if (TTF_SizeUTF8(font, candidate.c_str(), &width, &height) == 0 &&
            width <= max_width) {
            return candidate;
        }
    }
    return suffix;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool looks_like_episode(const MediaEntry& entry) {
    if (entry.kind != MediaKind::video || entry.explicit_movie) {
        return false;
    }
    if (!entry.series_root.empty()) {
        return true;
    }
    const std::string value = lower_ascii(entry.path);
    if (value.find("/season ") != std::string::npos ||
        value.find("\\season ") != std::string::npos) {
        return true;
    }
    for (std::size_t index = 0; index + 5 < value.size(); ++index) {
        if (value[index] == 's' && std::isdigit(static_cast<unsigned char>(value[index + 1])) &&
            std::isdigit(static_cast<unsigned char>(value[index + 2])) && value[index + 3] == 'e' &&
            std::isdigit(static_cast<unsigned char>(value[index + 4])) &&
            std::isdigit(static_cast<unsigned char>(value[index + 5]))) {
            return true;
        }
        if (std::isdigit(static_cast<unsigned char>(value[index])) &&
            value[index + 1] == 'x' &&
            std::isdigit(static_cast<unsigned char>(value[index + 2])) &&
            std::isdigit(static_cast<unsigned char>(value[index + 3]))) {
            return true;
        }
    }
    return false;
}

std::string parent_path(const std::string& path) {
    if (path.empty() || path == "/") {
        return "/";
    }
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return slash == 0 ? "/" : std::string{};
    }
    return path.substr(0, slash);
}

std::string series_group_key(const MediaEntry& entry) {
    if (!entry.series_root.empty()) {
        return entry.series_root;
    }
    return looks_like_episode(entry) ? parent_path(entry.path) : std::string{};
}

std::string series_display_name(const MediaEntry& entry) {
    if (!entry.series_title.empty()) {
        return entry.series_title;
    }
    const std::string group = series_group_key(entry);
    return group.empty()
        ? (entry.title.empty() ? entry.name : entry.title)
        : media_source_default_title(group);
}

std::string library_item_display_name(const MediaEntry& entry) {
    return series_group_key(entry).empty()
        ? (entry.title.empty() ? entry.name : entry.title)
        : series_display_name(entry);
}

bool library_item_name_less(
    const MediaEntry& left,
    const MediaEntry& right) {
    const std::string left_name = library_item_display_name(left);
    const std::string right_name = library_item_display_name(right);
    if (natural_path_less(left_name, right_name)) {
        return true;
    }
    if (natural_path_less(right_name, left_name)) {
        return false;
    }
    return natural_path_less(left.path, right.path);
}

const char* entry_kind_name(const MediaEntry& entry) {
    switch (entry.kind) {
        case MediaKind::video:
            return looks_like_episode(entry) ? "TV" : "MOVIE";
        case MediaKind::audio:
            return "AUDIO";
        case MediaKind::playlist:
            return "PLAYLIST";
        default:
            return "MEDIA";
    }
}

const char* filter_name(int filter) {
    switch (filter) {
        case 1:
            return "CONTINUE WATCHING";
        case 2:
            return "RECENTLY PLAYED";
        case 3:
            return "FAVORITES";
        case 4:
            return "MOVIES";
        case 5:
            return "TV SHOWS";
        case 6:
            return "MUSIC";
        case 7:
            return "PLAYLISTS";
        default:
            return "ALL MEDIA";
    }
}

bool matches_filter(const MediaEntry& entry, int filter) {
    switch (filter) {
        case 1:
            return entry.kind == MediaKind::video && !entry.completed &&
                   entry.resume_position_ms >= 10000 && entry.resume_duration_ms > 0 &&
                   entry.resume_position_ms < entry.resume_duration_ms;
        case 2:
            return entry.last_played_unix > 0;
        case 3:
            return entry.favorite;
        case 4:
            return entry.kind == MediaKind::video && !looks_like_episode(entry);
        case 5:
            return looks_like_episode(entry);
        case 6:
            return entry.kind == MediaKind::audio;
        case 7:
            return entry.kind == MediaKind::playlist;
        default:
            return true;
    }
}

std::string display_name(const MediaEntry& entry) {
    return entry.title.empty() ? entry.name : entry.title;
}

std::string format_duration(std::int64_t milliseconds) {
    if (milliseconds <= 0) {
        return {};
    }
    const std::int64_t total_seconds = milliseconds / 1000;
    const std::int64_t hours = total_seconds / 3600;
    const std::int64_t minutes = (total_seconds % 3600) / 60;
    if (hours > 0) {
        return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
    }
    return std::to_string(minutes) + "m";
}

std::string row_details(const MediaEntry& entry) {
    std::string details;
    if (entry.favorite) {
        details = "FAVORITE";
    }
    if (entry.completed) {
        if (!details.empty()) {
            details += "  |  ";
        }
        details += "WATCHED";
    } else if (entry.resume_position_ms > 0 && entry.resume_duration_ms > 0) {
        const long double raw_percent =
            static_cast<long double>(entry.resume_position_ms) * 100.0L /
            static_cast<long double>(entry.resume_duration_ms);
        const std::int64_t percent = static_cast<std::int64_t>(
            std::clamp(raw_percent, 0.0L, 99.0L));
        if (!details.empty()) {
            details += "  |  ";
        }
        details += std::to_string(percent) + "%";
    }
    if (entry.width > 0 && entry.height > 0) {
        if (!details.empty()) {
            details += "  |  ";
        }
        details += std::to_string(entry.width) + "x" + std::to_string(entry.height);
    }
    const std::int64_t duration = entry.duration_ms > 0
        ? entry.duration_ms
        : entry.resume_duration_ms;
    const std::string formatted_duration = format_duration(duration);
    if (!formatted_duration.empty()) {
        if (!details.empty()) {
            details += "  |  ";
        }
        details += formatted_duration;
    }
    return details;
}

bool is_directory(const std::string& path) {
    struct stat status {};
    return lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
           !S_ISLNK(status.st_mode);
}

bool fatal_browser_io_errno(int value) {
    if (value == EIO || value == EBADF || value == EFAULT) {
        return true;
    }
#ifdef ESTALE
    if (value == ESTALE) {
        return true;
    }
#endif
    return false;
}

} // namespace

struct LibraryUi::Impl {
    struct BrowserEntry {
        std::string name;
        std::string path;
        bool directory = false;
        MediaKind kind = MediaKind::unknown;
    };

    SDL_Renderer* renderer = nullptr;
    TTF_Font* title_font = nullptr;
    TTF_Font* row_font = nullptr;
    TTF_Font* footer_font = nullptr;
    SDL_mutex* mutex = nullptr;
    SDL_Thread* scan_thread = nullptr;
    SDL_Thread* import_thread = nullptr;
    SDL_Thread* thumbnail_thread = nullptr;
    std::vector<MediaEntry> entries;
    std::vector<MediaSource> sources;
    std::string active_series_root;
    std::string active_season_root;
    bool browser_mode = false;
    std::string browser_path;
    std::vector<BrowserEntry> browser_entries;
    int saved_library_selected = 0;
    int saved_library_first_visible = 0;
    std::array<bool, SDL_CONTROLLER_BUTTON_MAX> controller_buttons_down{};
    std::vector<Label> row_labels;
    std::vector<Label> browser_help_labels;
    Label title_label;
    Label status_label;
    Label notice_label;
    std::vector<FooterHint> footer_hints;
    Label artwork_label;
    Label empty_title_label;
    Label empty_help_label;
    bool empty_action_enabled = false;
    SDL_Texture* artwork_texture = nullptr;
    int artwork_width = 0;
    int artwork_height = 0;
    int artwork_generation = -1;
    std::string artwork_media_path;
    std::string artwork_path;
    std::string thumbnail_request_path;
    std::string thumbnail_error;
    VideoThumbnail thumbnail_result;
    std::atomic<bool> thumbnail_cancel{false};
    std::atomic<bool> import_cancel{false};
    bool thumbnail_loading = false;
    bool thumbnail_ready = false;
    bool artwork_is_video_frame = false;
    std::int64_t artwork_position_ms = 0;
    std::string last_error;
    std::string database_path = "/data/PS5-MediaCenter/library.db";
    int selected = 0;
    int first_visible = 0;
    int published_generation = 0;
    int rendered_generation = -1;
    int rendered_selected = -1;
    int rendered_first = -1;
    int filter = 0;
    int rendered_filter = -1;
    LibrarySortMode sort_mode = LibrarySortMode::smart;
    int rendered_sort_mode = -1;
    std::string search_query;
    std::string search_edit;
    std::string preferred_selected_path;
    std::string rendered_search_query;
    std::string rendered_search_edit;
    std::string notice;
    std::uint64_t notice_until = 0;
    std::string pending_remove_path;
    std::uint64_t pending_remove_until = 0;
    int notice_generation = 0;
    int rendered_notice_generation = -1;
    LibraryOverlay overlay = LibraryOverlay::none;
    int overlay_selected = 0;
    int rendered_overlay = -1;
    int rendered_overlay_selected = -1;
    PlayerSettings player_settings;
    bool search_editing = false;
    bool rendered_search_editing = false;
    bool ime_was_visible = false;
    bool scanning = false;
    bool cancel_scan = false;
    bool import_running = false;
    bool import_result_ready = false;
    BulkImportResult pending_import_result;
    std::string import_target;
    std::string import_progress_path;
    std::size_t import_direct_entries = 0;
    std::size_t import_entries_checked = 0;
    std::size_t import_movies_found = 0;
    std::size_t import_shows_found = 0;
    bool sort_setting_loaded = false;
    bool sort_setting_dirty = false;
    std::vector<std::pair<std::string, bool>> pending_favorites;
    mutable std::vector<std::size_t> filtered_cache;
    mutable int filtered_cache_generation = -1;
    mutable int filtered_cache_filter = -1;
    mutable int filtered_cache_sort_mode = -1;
    mutable std::string filtered_cache_query;
    mutable std::string filtered_cache_series_root;
    mutable std::string filtered_cache_season_root;

    void load_player_settings() {
        PlayerSettings loaded{};
        LibraryDatabase database;
        if (!database.open(database_path)) {
            player_settings = loaded;
            return;
        }
        std::string value;
        int parsed = 0;
        bool parsed_bool = false;
        if (database.get_setting(
                std::string(kSettingVolumePercent),
                value) &&
            parse_setting_integer(value, 0, 100, parsed)) {
            loaded.volume_percent = parsed;
        }
        if (database.get_setting(
                std::string(kSettingShortSeekSeconds),
                value) &&
            parse_setting_integer(value, 1, 300, parsed)) {
            loaded.short_seek_seconds = parsed;
        }
        if (database.get_setting(
                std::string(kSettingLongSeekSeconds),
                value) &&
            parse_setting_integer(value, 1, 900, parsed)) {
            loaded.long_seek_seconds = parsed;
        }
        if (database.get_setting(
                std::string(kSettingOsdDurationMs),
                value) &&
            parse_setting_integer(value, 500, 30000, parsed)) {
            loaded.osd_duration_ms = parsed;
        }
        if (database.get_setting(
                std::string(kSettingResumePlayback),
                value) &&
            parse_setting_boolean(value, parsed_bool)) {
            loaded.resume_playback = parsed_bool;
        }
        if (database.get_setting(
                std::string(kSettingAutoSubtitles),
                value) &&
            parse_setting_boolean(value, parsed_bool)) {
            loaded.auto_subtitles = parsed_bool;
        }
        player_settings = normalized_player_settings(loaded);
    }

    bool persist_player_settings() {
        const PlayerSettings snapshot =
            normalized_player_settings(player_settings);
        LibraryDatabase database;
        const bool saved =
            database.open(database_path) &&
            database.set_settings({
                {std::string(kSettingVolumePercent),
                 std::to_string(snapshot.volume_percent)},
                {std::string(kSettingShortSeekSeconds),
                 std::to_string(snapshot.short_seek_seconds)},
                {std::string(kSettingLongSeekSeconds),
                 std::to_string(snapshot.long_seek_seconds)},
                {std::string(kSettingOsdDurationMs),
                 std::to_string(snapshot.osd_duration_ms)},
                {std::string(kSettingResumePlayback),
                 snapshot.resume_playback ? "1" : "0"},
                {std::string(kSettingAutoSubtitles),
                 snapshot.auto_subtitles ? "1" : "0"},
            });
        if (!saved) {
            last_error =
                "Unable to save playback settings: " + database.error();
            diagnostics_log(
                DiagnosticLevel::error,
                "player-settings save failed error=%s",
                database.error().c_str());
            set_notice(last_error, 8000);
            return false;
        }
        player_settings = snapshot;
        diagnostics_log(
            DiagnosticLevel::info,
            "player-settings saved volume=%d short_seek=%d long_seek=%d osd_ms=%d resume=%d auto_subtitles=%d",
            snapshot.volume_percent,
            snapshot.short_seek_seconds,
            snapshot.long_seek_seconds,
            snapshot.osd_duration_ms,
            snapshot.resume_playback ? 1 : 0,
            snapshot.auto_subtitles ? 1 : 0);
        return true;
    }

    void open_overlay(LibraryOverlay next) {
        SDL_LockMutex(mutex);
        overlay = next;
        overlay_selected = 0;
        ++published_generation;
        SDL_UnlockMutex(mutex);
    }

    bool handle_overlay_button(Uint8 button) {
        LibraryOverlay current = LibraryOverlay::none;
        int current_selected = 0;
        SDL_LockMutex(mutex);
        current = overlay;
        current_selected = overlay_selected;
        SDL_UnlockMutex(mutex);
        if (current == LibraryOverlay::none) {
            return false;
        }
        if (button == kControllerOptionsButton) {
            open_overlay(LibraryOverlay::none);
            return false;
        }
        if (button == SDL_CONTROLLER_BUTTON_B) {
            open_overlay(
                current == LibraryOverlay::menu
                    ? LibraryOverlay::none
                    : LibraryOverlay::menu);
            return false;
        }

        const int item_count =
            current == LibraryOverlay::menu
                ? 6
                : (current == LibraryOverlay::settings ? 7 : 0);
        if (item_count > 0 &&
            (button == SDL_CONTROLLER_BUTTON_DPAD_UP ||
             button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
            const int delta =
                button == SDL_CONTROLLER_BUTTON_DPAD_UP ? -1 : 1;
            SDL_LockMutex(mutex);
            overlay_selected =
                (overlay_selected + item_count + delta) % item_count;
            ++published_generation;
            SDL_UnlockMutex(mutex);
            return false;
        }

        if (current == LibraryOverlay::menu &&
            button == SDL_CONTROLLER_BUTTON_A) {
            switch (current_selected) {
                case 0:
                    open_overlay(LibraryOverlay::none);
                    open_browser();
                    break;
                case 1: {
                    open_overlay(LibraryOverlay::none);
                    bool has_query = false;
                    SDL_LockMutex(mutex);
                    has_query = !search_query.empty();
                    SDL_UnlockMutex(mutex);
                    if (has_query) {
                        clear_search();
                    } else {
                        begin_search();
                    }
                    break;
                }
                case 2:
                    open_overlay(LibraryOverlay::controls);
                    break;
                case 3:
                    open_overlay(LibraryOverlay::settings);
                    break;
                case 4:
                    open_overlay(LibraryOverlay::about);
                    break;
                case 5:
                    return true;
                default:
                    break;
            }
            return false;
        }

        if (current != LibraryOverlay::settings) {
            return false;
        }
        if (button == SDL_CONTROLLER_BUTTON_X) {
            const PlayerSettings previous = player_settings;
            player_settings = {};
            if (!persist_player_settings()) {
                player_settings = previous;
                set_notice("Unable to save playback settings", 8000);
            }
            SDL_LockMutex(mutex);
            ++published_generation;
            SDL_UnlockMutex(mutex);
            return false;
        }
        if (button != SDL_CONTROLLER_BUTTON_A &&
            button != SDL_CONTROLLER_BUTTON_DPAD_LEFT &&
            button != SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
            return false;
        }
        const int direction =
            button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : 1;
        const PlayerSettings previous = player_settings;
        switch (current_selected) {
            case 0:
                player_settings.volume_percent =
                    std::clamp(
                        player_settings.volume_percent + direction * 5,
                        0,
                        100);
                break;
            case 1:
                player_settings.short_seek_seconds =
                    next_short_seek_seconds(
                        player_settings.short_seek_seconds,
                        direction);
                break;
            case 2:
                player_settings.long_seek_seconds =
                    next_long_seek_seconds(
                        player_settings.long_seek_seconds,
                        direction);
                break;
            case 3:
                player_settings.osd_duration_ms =
                    next_osd_duration_ms(
                        player_settings.osd_duration_ms,
                        direction);
                break;
            case 4:
                player_settings.resume_playback =
                    !player_settings.resume_playback;
                break;
            case 5:
                player_settings.auto_subtitles =
                    !player_settings.auto_subtitles;
                break;
            case 6:
                player_settings = {};
                break;
            default:
                return false;
        }
        if (!persist_player_settings()) {
            player_settings = previous;
            set_notice("Unable to save playback settings", 8000);
        }
        SDL_LockMutex(mutex);
        ++published_generation;
        SDL_UnlockMutex(mutex);
        return false;
    }

    const std::vector<std::size_t>& filtered_indices_locked() const {
        if (filtered_cache_generation == published_generation &&
            filtered_cache_filter == filter &&
            filtered_cache_sort_mode == static_cast<int>(sort_mode) &&
            filtered_cache_query == search_query &&
            filtered_cache_series_root == active_series_root &&
            filtered_cache_season_root == active_season_root) {
            return filtered_cache;
        }

        filtered_cache.clear();
        filtered_cache.reserve(entries.size());
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const MediaEntry& entry = entries[index];
            const bool in_series =
                !active_series_root.empty() &&
                series_group_key(entry) == active_series_root;
            const bool in_season =
                active_season_root.empty() ||
                media_season_root(entry, active_series_root) ==
                    active_season_root;
            if ((active_series_root.empty()
                     ? matches_filter(entry, filter)
                     : in_series && in_season) &&
                media_matches_normalized_query(entry, search_query)) {
                filtered_cache.push_back(index);
            }
        }
        sort_media_indices(
            entries,
            filtered_cache,
            active_series_root.empty() ? sort_mode : LibrarySortMode::name,
            active_series_root.empty() && (filter == 1 || filter == 2));
        if (active_series_root.empty()) {
            std::unordered_set<std::string> seen_series;
            filtered_cache.erase(
                std::remove_if(
                    filtered_cache.begin(),
                    filtered_cache.end(),
                    [&](std::size_t index) {
                        const std::string group = series_group_key(entries[index]);
                        return !group.empty() && !seen_series.insert(group).second;
                    }),
                filtered_cache.end());
            const bool display_name_sort =
                sort_mode == LibrarySortMode::name ||
                (sort_mode == LibrarySortMode::smart &&
                 filter != 1 && filter != 2);
            if (display_name_sort) {
                std::stable_sort(
                    filtered_cache.begin(),
                    filtered_cache.end(),
                    [&](std::size_t left, std::size_t right) {
                        return library_item_name_less(
                            entries[left], entries[right]);
                    });
            }
        } else if (active_season_root.empty()) {
            std::unordered_set<std::string> seen_seasons;
            filtered_cache.erase(
                std::remove_if(
                    filtered_cache.begin(),
                    filtered_cache.end(),
                    [&](std::size_t index) {
                        const std::string season =
                            media_season_root(
                                entries[index], active_series_root);
                        return !season.empty() &&
                            !seen_seasons.insert(season).second;
                    }),
                filtered_cache.end());
        }
        filtered_cache_generation = published_generation;
        filtered_cache_filter = filter;
        filtered_cache_sort_mode = static_cast<int>(sort_mode);
        filtered_cache_query = search_query;
        filtered_cache_series_root = active_series_root;
        filtered_cache_season_root = active_season_root;
        return filtered_cache;
    }

    std::string selected_path_locked() const {
        const std::vector<std::size_t>& filtered = filtered_indices_locked();
        if (selected < 0 || selected >= static_cast<int>(filtered.size())) {
            return {};
        }
        return entries[filtered[static_cast<std::size_t>(selected)]].path;
    }

    void restore_selected_path_locked(const std::string& path) {
        const std::vector<std::size_t>& filtered = filtered_indices_locked();
        if (!path.empty()) {
            for (std::size_t position = 0; position < filtered.size(); ++position) {
                if (entries[filtered[position]].path == path) {
                    selected = static_cast<int>(position);
                    first_visible = std::clamp(
                        first_visible,
                        std::max(0, selected - kVisibleRows + 1),
                        selected);
                    return;
                }
            }
        }
        selected = std::clamp(selected, 0, std::max(0, static_cast<int>(filtered.size()) - 1));
        first_visible = std::clamp(
            first_visible, 0, std::max(0, static_cast<int>(filtered.size()) - kVisibleRows));
    }

    void begin_search() {
        SDL_LockMutex(mutex);
        if (search_editing) {
            SDL_UnlockMutex(mutex);
            return;
        }
        search_edit.clear();
        search_editing = true;
        ime_was_visible = false;
        SDL_UnlockMutex(mutex);
        SDL_StartTextInput();
    }

    void finish_search(bool commit) {
        SDL_LockMutex(mutex);
        const std::string previous_path = selected_path_locked();
        if (commit) {
            search_query = normalize_media_query(search_edit);
        }
        search_edit.clear();
        search_editing = false;
        ime_was_visible = false;
        restore_selected_path_locked(previous_path);
        SDL_UnlockMutex(mutex);
        SDL_StopTextInput();
    }

    void clear_search() {
        SDL_LockMutex(mutex);
        const std::string previous_path = selected_path_locked();
        search_query.clear();
        search_edit.clear();
        search_editing = false;
        ime_was_visible = false;
        restore_selected_path_locked(previous_path);
        SDL_UnlockMutex(mutex);
        SDL_StopTextInput();
    }

    bool load_browser_directory(const std::string& requested_path) {
        const std::string path = normalize_media_source_path(requested_path);
        struct stat root_status {};
        errno = 0;
        const bool root_exists =
            !path.empty() && lstat(path.c_str(), &root_status) == 0;
        if (!root_exists || !S_ISDIR(root_status.st_mode) ||
            S_ISLNK(root_status.st_mode)) {
            const int value =
                !root_exists && errno != 0 ? errno : ENOTDIR;
            last_error = "Unable to open folder: " +
                (path.empty() ? std::string("<empty>") : path) +
                " (errno " + std::to_string(value) + ")";
            return false;
        }
        int open_flags = O_RDONLY;
#ifdef O_CLOEXEC
        open_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        open_flags |= O_NOFOLLOW;
#endif
#ifdef O_DIRECTORY
        open_flags |= O_DIRECTORY;
#endif
        const int descriptor = ::open(path.c_str(), open_flags);
        if (descriptor < 0) {
            last_error = "Unable to open folder: " + path +
                " (errno " + std::to_string(errno) + ")";
            return false;
        }
        struct stat opened_status {};
        errno = 0;
        const int stat_result = fstat(descriptor, &opened_status);
        const int stat_errno = stat_result != 0 && errno != 0
            ? errno
            : ESTALE;
        if (stat_result != 0 ||
            opened_status.st_dev != root_status.st_dev ||
            opened_status.st_ino != root_status.st_ino ||
            !S_ISDIR(opened_status.st_mode)) {
            ::close(descriptor);
            last_error = "Folder changed while opening: " + path +
                " (errno " + std::to_string(stat_errno) + ")";
            return false;
        }
        DIR* directory = fdopendir(descriptor);
        if (!directory) {
            const int value = errno;
            ::close(descriptor);
            last_error = "Unable to open folder: " + path +
                " (errno " + std::to_string(value) + ")";
            return false;
        }

        std::vector<BrowserEntry> updated;
        int read_error = 0;
        int entry_error = 0;
        std::string entry_error_path;
        std::size_t entries_seen = 0;
        std::size_t directory_items = 0;
        std::size_t media_items = 0;
        std::size_t stat_fallbacks = 0;
        std::size_t unreadable_entries = 0;
        std::size_t skipped_symlinks = 0;
        std::size_t filtered_entries = 0;
        std::size_t nonregular_entries = 0;
        for (;;) {
            errno = 0;
            dirent* item = readdir(directory);
            if (!item) {
                read_error = errno;
                break;
            }
            if (std::strcmp(item->d_name, ".") == 0 ||
                std::strcmp(item->d_name, "..") == 0) {
                continue;
            }
            if (++entries_seen > kMaxBrowserEntriesSeen) {
                entry_error = EOVERFLOW;
                entry_error_path = path;
                break;
            }
            std::string item_path = path;
            if (item_path != "/") {
                item_path.push_back('/');
            }
            item_path += item->d_name;
            if (item_path.size() > kMaxBrowserPathBytes) {
                continue;
            }
            struct stat status {};
            if (fstatat(
                    descriptor,
                    item->d_name,
                    &status,
                    AT_SYMLINK_NOFOLLOW) != 0) {
                int value = errno;
                if (fatal_browser_io_errno(value) ||
                    lstat(item_path.c_str(), &status) != 0) {
                    if (!fatal_browser_io_errno(value)) {
                        value = errno;
                    }
                    if (!fatal_browser_io_errno(value)) {
                        ++unreadable_entries;
                        continue;
                    }
                    entry_error = value != 0 ? value : EIO;
                    entry_error_path = item_path;
                    break;
                }
                ++stat_fallbacks;
            }
            if (S_ISLNK(status.st_mode)) {
                ++skipped_symlinks;
                continue;
            }
            // This is an interactive one-directory-at-a-time picker. Mount
            // points must stay visible at `/`; opening one captures and
            // validates that mount as the next browsing root. Recursive media
            // scans retain their strict same-st_dev boundary.
            if (S_ISDIR(status.st_mode)) {
                const std::string folded = lower_ascii(item->d_name);
                if (folded == "$recycle.bin" ||
                    folded == "system volume information" ||
                    folded == ".spotlight-v100" ||
                    folded == ".trashes" ||
                    folded == "lost+found") {
                    ++filtered_entries;
                    continue;
                }
                updated.push_back({
                    item->d_name,
                    std::move(item_path),
                    true,
                    MediaKind::unknown});
                ++directory_items;
                if (updated.size() > kMaxBrowserItems) {
                    entry_error = EOVERFLOW;
                    entry_error_path = path;
                    break;
                }
                continue;
            }
            if (!S_ISREG(status.st_mode)) {
                ++nonregular_entries;
                continue;
            }
            const MediaKind kind = classify_media_path(item_path);
            if (kind == MediaKind::video || kind == MediaKind::audio ||
                kind == MediaKind::playlist) {
                updated.push_back({
                    item->d_name,
                    std::move(item_path),
                    false,
                    kind});
                ++media_items;
                if (updated.size() > kMaxBrowserItems) {
                    entry_error = EOVERFLOW;
                    entry_error_path = path;
                    break;
                }
            } else {
                ++filtered_entries;
            }
        }
        const int close_error = closedir(directory) == 0 ? 0 : errno;
        if (entry_error != 0) {
            last_error =
                (entry_error == EOVERFLOW
                     ? "Folder is too large to browse safely: "
                     : "Folder entry read failed: ") +
                (entry_error_path.empty() ? path : entry_error_path) +
                " (errno " + std::to_string(entry_error) + ")";
            return false;
        }
        if (read_error != 0) {
            last_error = "Folder read failed: " + path +
                " (errno " + std::to_string(read_error) + ")";
            return false;
        }
        if (close_error != 0) {
            last_error = "Folder close failed: " + path +
                " (errno " + std::to_string(close_error) + ")";
            return false;
        }
        std::sort(
            updated.begin(),
            updated.end(),
            [](const BrowserEntry& left, const BrowserEntry& right) {
                if (left.directory != right.directory) {
                    return left.directory;
                }
                return natural_path_less(left.name, right.name);
            });

        const std::size_t updated_count = updated.size();
        SDL_LockMutex(mutex);
        browser_path = path;
        browser_entries = std::move(updated);
        selected = 0;
        first_visible = 0;
        ++published_generation;
        SDL_UnlockMutex(mutex);
        diagnostics_log(
            DiagnosticLevel::info,
            "source-browser folder=%s shown=%zu entries_seen=%zu directories=%zu media=%zu stat_fallbacks=%zu unreadable=%zu symlinks=%zu filtered=%zu nonregular=%zu",
            path.c_str(),
            updated_count,
            entries_seen,
            directory_items,
            media_items,
            stat_fallbacks,
            unreadable_entries,
            skipped_symlinks,
            filtered_entries,
            nonregular_entries);
        return true;
    }

    void report_browser_failure() {
        diagnostics_log(
            DiagnosticLevel::error,
            "source-browser failed error=%s",
            last_error.c_str());
        set_notice("ADD MEDIA FAILED  |  " + last_error, 8000);
    }

    void open_browser() {
        SDL_LockMutex(mutex);
        saved_library_selected = selected;
        saved_library_first_visible = first_visible;
        browser_mode = true;
        const std::string initial = "/";
        SDL_UnlockMutex(mutex);
        if (!load_browser_directory(initial)) {
            SDL_LockMutex(mutex);
            browser_mode = false;
            selected = saved_library_selected;
            first_visible = saved_library_first_visible;
            ++published_generation;
            SDL_UnlockMutex(mutex);
            report_browser_failure();
        }
    }

    void close_browser() {
        SDL_LockMutex(mutex);
        browser_mode = false;
        browser_entries.clear();
        browser_path.clear();
        selected = saved_library_selected;
        first_visible = saved_library_first_visible;
        ++published_generation;
        SDL_UnlockMutex(mutex);
    }

    bool persist_sources() {
        std::vector<MediaSource> snapshot;
        SDL_LockMutex(mutex);
        snapshot = sources;
        SDL_UnlockMutex(mutex);
        LibraryDatabase database;
        if (!database.open(database_path) ||
            !database.set_setting(
                "library.media_sources.v1",
                serialize_media_sources(snapshot))) {
            last_error = "Unable to save media sources: " + database.error();
            diagnostics_log(
                DiagnosticLevel::error,
                "media-source save failed error=%s",
                database.error().c_str());
            return false;
        }
        return true;
    }

    void set_notice(std::string message, std::uint64_t milliseconds = 5000) {
        SDL_LockMutex(mutex);
        notice = std::move(message);
        const std::uint64_t now = SDL_GetTicks64();
        notice_until = now > UINT64_MAX - milliseconds
            ? UINT64_MAX
            : now + milliseconds;
        ++notice_generation;
        ++published_generation;
        SDL_UnlockMutex(mutex);
    }

    void add_sources(
        const std::vector<MediaSource>& additions,
        const std::string& success_message) {
        stop_scan();
        std::size_t inserted = 0;
        std::vector<MediaSource> previous_sources;
        SDL_LockMutex(mutex);
        previous_sources = sources;
        for (MediaSource source : additions) {
            source.path = normalize_media_source_path(std::move(source.path));
            if (source.path.empty() || source.path[0] != '/') {
                continue;
            }
            if (source.title.empty()) {
                source.title = media_source_default_title(source.path);
            }
            const auto duplicate = std::find_if(
                sources.begin(),
                sources.end(),
                [&](const MediaSource& current) {
                    return current.kind == source.kind &&
                           current.path == source.path;
                });
            if (duplicate == sources.end()) {
                sources.push_back(std::move(source));
                ++inserted;
            }
        }
        annotate_media_sources(entries, sources);
        SDL_UnlockMutex(mutex);
        if (!persist_sources()) {
            SDL_LockMutex(mutex);
            sources = std::move(previous_sources);
            annotate_media_sources(entries, sources);
            SDL_UnlockMutex(mutex);
            set_notice("IMPORT FAILED  |  " + last_error, 8000);
            (void)start_scan();
            return;
        }
        diagnostics_log(
            DiagnosticLevel::info,
            "media-source batch added=%zu requested=%zu",
            inserted,
            additions.size());
        close_browser();
        set_notice(
            success_message + "  |  " + std::to_string(inserted) +
            " NEW SOURCES");
        start_scan();
    }

    void finish_bulk_import(
        const std::string& target,
        const BulkImportResult& result) {
        if (!result.ok()) {
            diagnostics_log(
                DiagnosticLevel::error,
                "bulk-import failed root=%s path=%s errno=%d",
                target.c_str(),
                result.fatal_path.c_str(),
                result.fatal_errno);
            set_notice(
                "Import failed (error " +
                std::to_string(result.fatal_errno) + "): " +
                (result.fatal_path.empty() ? target : result.fatal_path),
                8000);
            (void)start_scan();
            return;
        }
        if (result.sources.empty()) {
            diagnostics_log(
                DiagnosticLevel::warning,
                "bulk-import empty root=%s checked=%zu stat_fallbacks=%zu unreadable=%zu symlinks=%zu devices=%zu",
                target.c_str(),
                result.entries_checked,
                result.stat_fallbacks,
                result.unreadable_entries,
                result.skipped_symlinks,
                result.skipped_devices);
            set_notice(
                result.unreadable_entries > 0
                    ? "Nothing imported; some folder entries could not be read"
                    : "No movies or TV-show folders were found in " +
                          media_source_default_title(target),
                8000);
            (void)start_scan();
            return;
        }
        diagnostics_log(
            DiagnosticLevel::info,
            "bulk-import complete root=%s movies=%zu shows=%zu checked=%zu stat_fallbacks=%zu unreadable=%zu symlinks=%zu devices=%zu",
            target.c_str(),
            result.loose_movies,
            result.tv_folders,
            result.entries_checked,
            result.stat_fallbacks,
            result.unreadable_entries,
            result.skipped_symlinks,
            result.skipped_devices);
        add_sources(
            result.sources,
            "IMPORTED " + std::to_string(result.loose_movies) +
            " MOVIES + " + std::to_string(result.tv_folders) +
            " TV SHOWS");
    }

    static int bulk_import_entry(void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        std::string target;
        SDL_LockMutex(self->mutex);
        target = self->import_target;
        SDL_UnlockMutex(self->mutex);

        BulkImportResult result = discover_bulk_media_sources(
            target,
            [&]() {
                return self->import_cancel.load(std::memory_order_relaxed);
            },
            [&](const BulkImportProgress& progress) {
                SDL_LockMutex(self->mutex);
                self->import_progress_path = progress.current_path;
                self->import_direct_entries =
                    progress.direct_entries_checked;
                self->import_entries_checked = progress.entries_checked;
                self->import_movies_found = progress.loose_movies;
                self->import_shows_found = progress.tv_folders;
                ++self->published_generation;
                SDL_UnlockMutex(self->mutex);
            });

        SDL_LockMutex(self->mutex);
        self->pending_import_result = std::move(result);
        self->import_running = false;
        self->import_result_ready = true;
        ++self->published_generation;
        SDL_UnlockMutex(self->mutex);
        return 0;
    }

    void consume_bulk_import_result() {
        BulkImportResult result;
        std::string target;
        SDL_Thread* completed_thread = nullptr;
        SDL_LockMutex(mutex);
        if (import_result_ready) {
            result = std::move(pending_import_result);
            pending_import_result = {};
            target = import_target;
            import_result_ready = false;
            completed_thread = import_thread;
            import_thread = nullptr;
        }
        SDL_UnlockMutex(mutex);
        if (!completed_thread) {
            return;
        }
        SDL_WaitThread(completed_thread, nullptr);
        if (result.cancelled) {
            diagnostics_log(
                DiagnosticLevel::info,
                "bulk-import cancelled root=%s checked=%zu movies=%zu shows=%zu",
                target.c_str(),
                result.entries_checked,
                result.loose_movies,
                result.tv_folders);
            set_notice("Library import canceled", 5000);
            (void)start_scan();
            return;
        }
        finish_bulk_import(target, result);
    }

    void stop_bulk_import() {
        import_cancel.store(true, std::memory_order_relaxed);
        if (import_thread) {
            SDL_WaitThread(import_thread, nullptr);
            import_thread = nullptr;
        }
        if (mutex) {
            SDL_LockMutex(mutex);
            import_running = false;
            import_result_ready = false;
            pending_import_result = {};
            import_target.clear();
            import_progress_path.clear();
            import_direct_entries = 0;
            import_entries_checked = 0;
            import_movies_found = 0;
            import_shows_found = 0;
            SDL_UnlockMutex(mutex);
        }
        import_cancel.store(false, std::memory_order_relaxed);
    }

    void bulk_import_folder(const std::string& target) {
        if (target == "/") {
            set_notice("Choose a media folder before using Import Library");
            return;
        }
        SDL_LockMutex(mutex);
        const bool already_running = import_running;
        const bool result_pending = import_result_ready;
        SDL_UnlockMutex(mutex);
        if (already_running) {
            import_cancel.store(true, std::memory_order_relaxed);
            diagnostics_log(
                DiagnosticLevel::info,
                "bulk-import cancel requested root=%s",
                import_target.c_str());
            set_notice("Canceling library import...", 5000);
            return;
        }
        if (result_pending || import_thread) {
            set_notice("Finishing the previous library import...", 5000);
            return;
        }

        stop_scan();
        diagnostics_log(
            DiagnosticLevel::info,
            "bulk-import begin root=%s mode=background",
            target.c_str());
        set_notice(
            "Checking " + media_source_default_title(target) +
            " for movies and TV shows...",
            30000);
        import_cancel.store(false, std::memory_order_relaxed);
        SDL_LockMutex(mutex);
        import_target = target;
        import_progress_path = target;
        import_direct_entries = 0;
        import_entries_checked = 0;
        import_movies_found = 0;
        import_shows_found = 0;
        pending_import_result = {};
        import_result_ready = false;
        import_running = true;
        ++published_generation;
        SDL_UnlockMutex(mutex);
        import_thread = SDL_CreateThread(
            bulk_import_entry,
            "ps5mc-bulk-import",
            this);
        if (!import_thread) {
            SDL_LockMutex(mutex);
            import_running = false;
            ++published_generation;
            SDL_UnlockMutex(mutex);
            last_error =
                std::string("Unable to start library import: ") +
                SDL_GetError();
            diagnostics_log(
                DiagnosticLevel::error,
                "bulk-import thread create failed error=%s",
                SDL_GetError());
            set_notice(last_error, 8000);
            (void)start_scan();
        }
    }

    void add_source(MediaSourceKind kind, const std::string& raw_path) {
        SDL_LockMutex(mutex);
        const bool import_active =
            import_running || import_result_ready || import_thread != nullptr;
        SDL_UnlockMutex(mutex);
        if (import_active) {
            set_notice(
                "Finish or cancel the current library import first",
                5000);
            return;
        }
        stop_scan();
        const std::string path = normalize_media_source_path(raw_path);
        struct stat status {};
        errno = 0;
        const bool valid =
            lstat(path.c_str(), &status) == 0 &&
            !S_ISLNK(status.st_mode) &&
            ((kind == MediaSourceKind::tv_folder && S_ISDIR(status.st_mode)) ||
             (kind == MediaSourceKind::movie_file && S_ISREG(status.st_mode) &&
              classify_media_path(path) != MediaKind::unknown &&
              classify_media_path(path) != MediaKind::subtitle));
        const int validation_errno = errno;
        if (!valid) {
            last_error = "Selected media source is no longer available";
            diagnostics_log(
                DiagnosticLevel::error,
                "media-source invalid kind=%s path=%s errno=%d",
                kind == MediaSourceKind::tv_folder
                    ? "tv-folder"
                    : "movie-file",
                path.c_str(),
                validation_errno);
            report_browser_failure();
            (void)start_scan();
            return;
        }

        std::vector<MediaSource> previous_sources;
        SDL_LockMutex(mutex);
        previous_sources = sources;
        const auto duplicate = std::find_if(
            sources.begin(),
            sources.end(),
            [&](const MediaSource& source) {
                return source.kind == kind && source.path == path;
            });
        const bool inserted = duplicate == sources.end();
        const std::string title = media_source_default_title(path);
        if (inserted) {
            sources.push_back({kind, path, title});
        }
        annotate_media_sources(entries, sources);
        notice =
            (inserted ? "Added " : "Already added ") +
            std::string(kind == MediaSourceKind::tv_folder ? "TV Show: " : "Movie: ") +
            title;
        const std::uint64_t now = SDL_GetTicks64();
        notice_until = now > UINT64_MAX - 5000 ? UINT64_MAX : now + 5000;
        ++notice_generation;
        ++published_generation;
        SDL_UnlockMutex(mutex);
        if (!persist_sources()) {
            SDL_LockMutex(mutex);
            sources = std::move(previous_sources);
            annotate_media_sources(entries, sources);
            SDL_UnlockMutex(mutex);
            report_browser_failure();
            (void)start_scan();
            return;
        }
        diagnostics_log(
            DiagnosticLevel::info,
            "media-source added kind=%s path=%s",
            kind == MediaSourceKind::tv_folder ? "tv-folder" : "movie-file",
            path.c_str());
        close_browser();
        start_scan();
    }

    void remove_selected_source() {
        MediaSource target{};
        bool found = false;
        std::string display;
        SDL_LockMutex(mutex);
        const std::vector<std::size_t>& filtered = filtered_indices_locked();
        if (selected >= 0 && selected < static_cast<int>(filtered.size())) {
            const MediaEntry& entry =
                entries[filtered[static_cast<std::size_t>(selected)]];
            const std::string group = series_group_key(entry);
            for (const MediaSource& source : sources) {
                const bool matches =
                    (source.kind == MediaSourceKind::movie_file &&
                     source.path == entry.path) ||
                    (source.kind == MediaSourceKind::tv_folder &&
                     source.path == group);
                if (matches) {
                    target = source;
                    display = source.title.empty()
                        ? media_source_default_title(source.path)
                        : source.title;
                    found = true;
                    break;
                }
            }
        }
        const std::uint64_t now = SDL_GetTicks64();
        if (!found) {
            SDL_UnlockMutex(mutex);
            set_notice("This item is not a configured library source");
            return;
        }
        if (pending_remove_path != target.path ||
            now >= pending_remove_until) {
            pending_remove_path = target.path;
            pending_remove_until =
                now > UINT64_MAX - 5000 ? UINT64_MAX : now + 5000;
            notice = "Press Triangle again to remove " + display;
            notice_until = pending_remove_until;
            ++notice_generation;
            ++published_generation;
            SDL_UnlockMutex(mutex);
            return;
        }
        pending_remove_path.clear();
        pending_remove_until = 0;
        SDL_UnlockMutex(mutex);

        stop_scan();
        std::vector<MediaSource> previous_sources;
        SDL_LockMutex(mutex);
        previous_sources = sources;
        sources.erase(
            std::remove_if(
                sources.begin(),
                sources.end(),
                [&](const MediaSource& source) {
                    return source.kind == target.kind &&
                           source.path == target.path;
                }),
            sources.end());
        active_series_root.clear();
        active_season_root.clear();
        SDL_UnlockMutex(mutex);
        if (!persist_sources()) {
            SDL_LockMutex(mutex);
            sources = std::move(previous_sources);
            SDL_UnlockMutex(mutex);
            set_notice("Unable to save library removal");
            (void)start_scan();
            return;
        }

        LibraryDatabase database;
        if (!database.open(database_path) ||
            !database.remove_root(target.path)) {
            SDL_LockMutex(mutex);
            sources = std::move(previous_sources);
            SDL_UnlockMutex(mutex);
            if (!persist_sources()) {
                diagnostics_log(
                    DiagnosticLevel::error,
                    "media-source removal rollback-save failed path=%s error=%s",
                    target.path.c_str(),
                    last_error.c_str());
                set_notice(
                    "Removal failed and source rollback could not be saved",
                    8000);
            } else {
                set_notice("Unable to remove source from library database");
            }
            (void)start_scan();
            return;
        }
        std::vector<MediaEntry> remaining = database.list_media();
        SDL_LockMutex(mutex);
        const std::vector<MediaSource> current_sources = sources;
        SDL_UnlockMutex(mutex);
        retain_configured_media(remaining, current_sources);
        annotate_media_sources(remaining, current_sources);
        publish(std::move(remaining));
        diagnostics_log(
            DiagnosticLevel::info,
            "media-source removed kind=%s path=%s",
            target.kind == MediaSourceKind::tv_folder
                ? "tv-folder"
                : "movie-file",
            target.path.c_str());
        set_notice("Removed from library: " + display);
        (void)start_scan();
    }

    void browser_parent() {
        std::string current;
        SDL_LockMutex(mutex);
        current = browser_path;
        SDL_UnlockMutex(mutex);
        if (current == "/") {
            close_browser();
            return;
        }
        if (!load_browser_directory(parent_path(current))) {
            report_browser_failure();
        }
    }

    void handle_browser_button(Uint8 button) {
        BrowserEntry highlighted;
        std::string current;
        bool has_highlighted = false;
        SDL_LockMutex(mutex);
        current = browser_path;
        if (selected >= 0 && selected < static_cast<int>(browser_entries.size())) {
            highlighted = browser_entries[static_cast<std::size_t>(selected)];
            has_highlighted = true;
        }
        SDL_UnlockMutex(mutex);

        switch (button) {
            case SDL_CONTROLLER_BUTTON_A:
                if (!has_highlighted) {
                    return;
                }
                if (highlighted.directory) {
                    if (!load_browser_directory(highlighted.path)) {
                        report_browser_failure();
                    }
                } else {
                    add_source(MediaSourceKind::movie_file, highlighted.path);
                }
                break;
            case SDL_CONTROLLER_BUTTON_Y:
                if (has_highlighted && highlighted.directory) {
                    add_source(MediaSourceKind::tv_folder, highlighted.path);
                }
                break;
            case SDL_CONTROLLER_BUTTON_X:
                bulk_import_folder(
                    has_highlighted && highlighted.directory
                        ? highlighted.path
                        : current);
                break;
            case SDL_CONTROLLER_BUTTON_B:
                browser_parent();
                break;
            case kControllerOptionsButton:
            case kControllerTouchpadButton:
                close_browser();
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: {
                int delta = 0;
                if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                    delta = -1;
                } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                    delta = 1;
                } else if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                    delta = -kVisibleRows;
                } else {
                    delta = kVisibleRows;
                }
                SDL_LockMutex(mutex);
                selected = std::clamp(
                    selected + delta,
                    0,
                    std::max(0, static_cast<int>(browser_entries.size()) - 1));
                ++published_generation;
                SDL_UnlockMutex(mutex);
                break;
            }
            default:
                break;
        }
    }

    void cycle_filter(int direction) {
        SDL_LockMutex(mutex);
        const std::string previous_path = selected_path_locked();
        filter = (filter + (direction < 0 ? kFilterCount - 1 : 1)) % kFilterCount;
        selected = 0;
        first_visible = 0;
        restore_selected_path_locked(previous_path);
        SDL_UnlockMutex(mutex);
    }

    void overlay_pending_favorites_locked(std::vector<MediaEntry>& target) const {
        for (MediaEntry& entry : target) {
            const auto pending = std::find_if(
                pending_favorites.begin(),
                pending_favorites.end(),
                [&](const auto& item) { return item.first == entry.path; });
            if (pending != pending_favorites.end()) {
                entry.favorite = pending->second;
            }
        }
    }

    void queue_favorite_locked(const std::string& path, bool favorite) {
        auto pending = std::find_if(
            pending_favorites.begin(),
            pending_favorites.end(),
            [&](const auto& item) { return item.first == path; });
        if (pending == pending_favorites.end()) {
            pending_favorites.emplace_back(path, favorite);
        } else {
            pending->second = favorite;
        }
    }

    void persist_pending_favorites(LibraryDatabase& database) {
        SDL_LockMutex(mutex);
        const std::vector<std::pair<std::string, bool>> pending = pending_favorites;
        SDL_UnlockMutex(mutex);
        for (const auto& item : pending) {
            if (!database.set_favorite(item.first, item.second)) {
                diagnostics_log(
                    DiagnosticLevel::error,
                    "library-favorite save failed path=%s error=%s",
                    item.first.c_str(),
                    database.error().c_str());
                continue;
            }
            SDL_LockMutex(mutex);
            const auto current = std::find_if(
                pending_favorites.begin(),
                pending_favorites.end(),
                [&](const auto& value) {
                    return value.first == item.first && value.second == item.second;
                });
            if (current != pending_favorites.end()) {
                pending_favorites.erase(current);
            }
            SDL_UnlockMutex(mutex);
        }
    }

    void toggle_selected_favorite() {
        bool scan_owns_database = false;
        SDL_LockMutex(mutex);
        const std::vector<std::size_t>& filtered = filtered_indices_locked();
        if (selected < 0 || selected >= static_cast<int>(filtered.size())) {
            SDL_UnlockMutex(mutex);
            return;
        }
        const std::string path = entries[filtered[static_cast<std::size_t>(selected)]].path;
        const bool favorite = !entries[filtered[static_cast<std::size_t>(selected)]].favorite;
        for (MediaEntry& entry : entries) {
            if (entry.path == path) {
                entry.favorite = favorite;
                break;
            }
        }
        queue_favorite_locked(path, favorite);
        scan_owns_database = scanning;
        ++published_generation;
        restore_selected_path_locked(path);
        SDL_UnlockMutex(mutex);

        if (!scan_owns_database) {
            LibraryDatabase database;
            if (database.open(database_path)) {
                persist_pending_favorites(database);
            }
        }
    }

    void cycle_sort_mode() {
        SDL_LockMutex(mutex);
        const std::vector<std::size_t>& previous_filtered =
            filtered_indices_locked();
        std::string previous_path;
        std::string previous_series;
        if (selected >= 0 &&
            selected < static_cast<int>(previous_filtered.size())) {
            const MediaEntry& previous_entry =
                entries[previous_filtered[static_cast<std::size_t>(selected)]];
            previous_path = previous_entry.path;
            if (active_series_root.empty()) {
                previous_series = series_group_key(previous_entry);
            }
        }
        sort_mode = next_library_sort_mode(sort_mode);
        sort_setting_dirty = true;
        const bool scan_owns_database = scanning;
        const std::vector<std::size_t>& sorted = filtered_indices_locked();
        bool restored = false;
        for (std::size_t position = 0; position < sorted.size(); ++position) {
            const MediaEntry& candidate = entries[sorted[position]];
            if ((!previous_series.empty() &&
                 series_group_key(candidate) == previous_series) ||
                (previous_series.empty() &&
                 candidate.path == previous_path)) {
                selected = static_cast<int>(position);
                first_visible = std::clamp(
                    first_visible,
                    std::max(0, selected - kVisibleRows + 1),
                    selected);
                restored = true;
                break;
            }
        }
        if (!restored) {
            selected = std::clamp(
                selected,
                0,
                std::max(0, static_cast<int>(sorted.size()) - 1));
        }
        SDL_UnlockMutex(mutex);

        if (!scan_owns_database) {
            LibraryDatabase database;
            if (database.open(database_path)) {
                persist_sort_setting(database);
            }
        }
    }

    void persist_sort_setting(LibraryDatabase& database) {
        SDL_LockMutex(mutex);
        const bool dirty = sort_setting_dirty;
        const LibrarySortMode value = sort_mode;
        SDL_UnlockMutex(mutex);
        if (!dirty) {
            return;
        }
        if (!database.set_setting("library.sort_mode", library_sort_mode_key(value))) {
            diagnostics_log(
                DiagnosticLevel::error,
                "library-sort save failed error=%s",
                database.error().c_str());
            return;
        }
        SDL_LockMutex(mutex);
        if (sort_mode == value) {
            sort_setting_dirty = false;
            sort_setting_loaded = true;
        }
        SDL_UnlockMutex(mutex);
    }

    void update_ime_state() {
#if defined(PS5MC_PS5)
        SDL_LockMutex(mutex);
        const bool editing = search_editing;
        SDL_UnlockMutex(mutex);
        if (!editing || !renderer) {
            return;
        }
        SDL_Window* window = SDL_RenderGetWindow(renderer);
        const bool shown = window && SDL_IsScreenKeyboardShown(window) == SDL_TRUE;
        bool cancel_finished_input = false;
        SDL_LockMutex(mutex);
        if (search_editing) {
            if (shown) {
                ime_was_visible = true;
            } else if (ime_was_visible) {
                // The PS5 SDL backend emits text + Return for OK, but emits no
                // event for cancel. Reaching here while still editing is cancel.
                search_edit.clear();
                search_editing = false;
                ime_was_visible = false;
                cancel_finished_input = true;
            }
        }
        SDL_UnlockMutex(mutex);
        if (cancel_finished_input) {
            SDL_StopTextInput();
        }
#endif
    }

    void clear_labels() {
        for (Label& label : row_labels) {
            destroy_label(label);
        }
        row_labels.clear();
        for (Label& label : browser_help_labels) {
            destroy_label(label);
        }
        browser_help_labels.clear();
        destroy_label(title_label);
        destroy_label(status_label);
        destroy_label(notice_label);
        for (FooterHint& hint : footer_hints) {
            destroy_label(hint.button_label);
            destroy_label(hint.action_label);
        }
        footer_hints.clear();
        destroy_label(artwork_label);
        destroy_label(empty_title_label);
        destroy_label(empty_help_label);
        empty_action_enabled = false;
    }

    static int thumbnail_entry(void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        std::string path;
        SDL_LockMutex(self->mutex);
        path = self->thumbnail_request_path;
        SDL_UnlockMutex(self->mutex);

        VideoThumbnail result;
        std::string error;
        const bool succeeded = extract_video_thumbnail(
            path,
            result,
            error,
            &self->thumbnail_cancel);

        SDL_LockMutex(self->mutex);
        if (!self->thumbnail_cancel.load(std::memory_order_relaxed) &&
            path == self->thumbnail_request_path) {
            self->thumbnail_result = succeeded
                ? std::move(result)
                : VideoThumbnail{};
            self->thumbnail_error = std::move(error);
            self->thumbnail_loading = false;
            self->thumbnail_ready = true;
            ++self->published_generation;
        }
        SDL_UnlockMutex(self->mutex);
        return 0;
    }

    void stop_thumbnail() {
        thumbnail_cancel.store(true, std::memory_order_relaxed);
        if (thumbnail_thread) {
            SDL_WaitThread(thumbnail_thread, nullptr);
            thumbnail_thread = nullptr;
        }
        if (mutex) {
            SDL_LockMutex(mutex);
            thumbnail_request_path.clear();
            thumbnail_error.clear();
            thumbnail_result = {};
            thumbnail_loading = false;
            thumbnail_ready = false;
            SDL_UnlockMutex(mutex);
        }
        thumbnail_cancel.store(false, std::memory_order_relaxed);
    }

    void start_thumbnail(const std::string& media_path) {
        stop_thumbnail();
        thumbnail_cancel.store(false, std::memory_order_relaxed);
        SDL_LockMutex(mutex);
        thumbnail_request_path = media_path;
        thumbnail_error.clear();
        thumbnail_result = {};
        thumbnail_loading = true;
        thumbnail_ready = false;
        SDL_UnlockMutex(mutex);
        thumbnail_thread = SDL_CreateThread(
            thumbnail_entry,
            "ps5mc-video-preview",
            this);
        if (!thumbnail_thread) {
            SDL_LockMutex(mutex);
            thumbnail_loading = false;
            thumbnail_error = SDL_GetError();
            ++published_generation;
            SDL_UnlockMutex(mutex);
            diagnostics_log(
                DiagnosticLevel::warning,
                "video-preview thread create failed error=%s",
                SDL_GetError());
        }
    }

    void consume_thumbnail_result() {
        VideoThumbnail result;
        std::string path;
        std::string error;
        SDL_Thread* completed_thread = nullptr;
        SDL_LockMutex(mutex);
        if (thumbnail_ready) {
            result = std::move(thumbnail_result);
            path = thumbnail_request_path;
            error = std::move(thumbnail_error);
            thumbnail_ready = false;
            completed_thread = thumbnail_thread;
            thumbnail_thread = nullptr;
        }
        SDL_UnlockMutex(mutex);
        if (!completed_thread) {
            return;
        }
        SDL_WaitThread(completed_thread, nullptr);
        if (path != artwork_media_path) {
            return;
        }
        if (result.rgba.empty() || result.width <= 0 || result.height <= 0) {
            diagnostics_log(
                DiagnosticLevel::warning,
                "video-preview unavailable path=%s error=%s",
                path.c_str(),
                error.c_str());
            return;
        }
        SDL_Texture* texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STATIC,
            result.width,
            result.height);
        if (!texture ||
            SDL_UpdateTexture(
                texture,
                nullptr,
                result.rgba.data(),
                result.width * 4) != 0) {
            diagnostics_log(
                DiagnosticLevel::warning,
                "video-preview texture failed path=%s error=%s",
                path.c_str(),
                SDL_GetError());
            SDL_DestroyTexture(texture);
            return;
        }
        SDL_DestroyTexture(artwork_texture);
        artwork_texture = texture;
        artwork_width = result.width;
        artwork_height = result.height;
        artwork_position_ms = result.position_ms;
        artwork_is_video_frame = true;
        artwork_path.clear();
    }

    void clear_artwork() {
        stop_thumbnail();
        SDL_DestroyTexture(artwork_texture);
        artwork_texture = nullptr;
        artwork_width = 0;
        artwork_height = 0;
        artwork_generation = -1;
        artwork_media_path.clear();
        artwork_path.clear();
        artwork_is_video_frame = false;
        artwork_position_ms = 0;
    }

    void update_artwork(
        const std::string& media_path,
        int generation) {
        if (media_path == artwork_media_path) {
            return;
        }
        clear_artwork();
        artwork_media_path = media_path;
        artwork_generation = generation;
        if (media_path.empty()) {
            return;
        }

        artwork_path = find_local_artwork(media_path);
        if (artwork_path.empty()) {
            if (classify_media_path(media_path) == MediaKind::video) {
                start_thumbnail(media_path);
            }
            return;
        }
        ArtworkData artwork;
        std::string artwork_error;
        if (!load_local_artwork(artwork_path, artwork, artwork_error)) {
            diagnostics_log(
                DiagnosticLevel::warning,
                "library-artwork load failed path=%s error=%s",
                artwork_path.c_str(),
                artwork_error.c_str());
            artwork_path.clear();
            start_thumbnail(media_path);
            return;
        }
        SDL_RWops* source = SDL_RWFromConstMem(
            artwork.encoded.data(),
            static_cast<int>(artwork.encoded.size()));
        if (!source) {
            diagnostics_log(
                DiagnosticLevel::warning,
                "library-artwork rwops failed error=%s",
                SDL_GetError());
            artwork_path.clear();
            start_thumbnail(media_path);
            return;
        }
        SDL_Surface* surface = artwork.format == ArtworkFormat::png
            ? IMG_LoadPNG_RW(source)
            : IMG_LoadJPG_RW(source);
        SDL_RWclose(source);
        if (!surface) {
            diagnostics_log(
                DiagnosticLevel::warning,
                "library-artwork decode failed path=%s error=%s",
                artwork_path.c_str(),
                IMG_GetError());
            artwork_path.clear();
            start_thumbnail(media_path);
            return;
        }
        if (surface->w <= 0 || surface->h <= 0 ||
            static_cast<std::uint32_t>(surface->w) != artwork.width ||
            static_cast<std::uint32_t>(surface->h) != artwork.height) {
            diagnostics_log(
                DiagnosticLevel::warning,
                "library-artwork dimensions changed path=%s expected=%ux%u actual=%dx%d",
                artwork_path.c_str(),
                artwork.width,
                artwork.height,
                surface->w,
                surface->h);
            SDL_FreeSurface(surface);
            artwork_path.clear();
            start_thumbnail(media_path);
            return;
        }
        artwork_texture = SDL_CreateTextureFromSurface(renderer, surface);
        artwork_width = surface->w;
        artwork_height = surface->h;
        SDL_FreeSurface(surface);
        if (!artwork_texture) {
            diagnostics_log(
                DiagnosticLevel::warning,
                "library-artwork texture failed error=%s",
                SDL_GetError());
            artwork_path.clear();
            artwork_width = 0;
            artwork_height = 0;
            start_thumbnail(media_path);
        }
    }

    void publish(std::vector<MediaEntry> updated) {
        SDL_LockMutex(mutex);
        const std::string previous_path = selected_path_locked();
        entries = std::move(updated);
        overlay_pending_favorites_locked(entries);
        ++published_generation;
        restore_selected_path_locked(previous_path);
        scanning = false;
        SDL_UnlockMutex(mutex);
    }

    static int scan_entry(void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        std::vector<MediaEntry> fallback_entries;
        std::vector<MediaSource> source_snapshot;
        SDL_LockMutex(self->mutex);
        source_snapshot = self->sources;
        SDL_UnlockMutex(self->mutex);
        LibraryDatabase database;
        const bool database_ready = database.open(self->database_path);
        if (!database_ready) {
            diagnostics_log(
                DiagnosticLevel::error,
                "library-database open failed path=%s error=%s",
                self->database_path.c_str(),
                database.error().c_str());
        }

        const ScanLimits limits{32, 200000, 75000};
        std::size_t metadata_probes_remaining = kMetadataProbeBudget;
        std::size_t metadata_probe_failures = 0;
        int fatal_probe_errno = 0;
        std::string fatal_probe_path;
        bool all_scans_complete = true;
        std::vector<std::string> scan_roots;
        for (const MediaSource& source : source_snapshot) {
            if (source.kind != MediaSourceKind::tv_folder ||
                !is_directory(source.path)) {
                continue;
            }
            const bool duplicate = std::any_of(
                scan_roots.begin(),
                scan_roots.end(),
                [&](const std::string& root) {
                    return source.path == root;
                });
            if (!duplicate) {
                scan_roots.push_back(source.path);
            }
        }
        for (const std::string& root : scan_roots) {
            SDL_LockMutex(self->mutex);
            const bool cancelled = self->cancel_scan;
            SDL_UnlockMutex(self->mutex);
            if (cancelled) {
                all_scans_complete = false;
                break;
            }
            if (!is_directory(root)) {
                continue;
            }

            diagnostics_log(
                DiagnosticLevel::info,
                "library-scan begin root=%s database=%d",
                root.c_str(),
                database_ready ? 1 : 0);

            bool database_scan = database_ready && database.begin_scan(root);
            bool write_ok = true;
            const ScanResult result = scan_media_library(
                root,
                [&](const MediaEntry& entry) {
                    fallback_entries.push_back(entry);
                    if (database_scan) {
                        const bool should_probe =
                            metadata_probes_remaining > 0 &&
                            metadata_probe_failures < kMetadataFailureBudget &&
                            (entry.kind == MediaKind::video || entry.kind == MediaKind::audio) &&
                            database.media_needs_metadata_probe(entry);
                        if (!database.upsert_media(entry)) {
                            write_ok = false;
                            return false;
                        }
                        if (should_probe) {
                            --metadata_probes_remaining;
                            MediaEntry enriched = entry;
                            std::string probe_error;
                            int probe_io_error = 0;
                            if (!probe_media_metadata(
                                    enriched,
                                    probe_error,
                                    &probe_io_error)) {
                                if (probe_io_error != 0) {
                                    fatal_probe_errno = probe_io_error;
                                    fatal_probe_path = entry.path;
                                    write_ok = false;
                                    return false;
                                }
                                ++metadata_probe_failures;
                                enriched.container = "unreadable";
                                diagnostics_log(
                                    DiagnosticLevel::warning,
                                    "metadata-probe skipped path=%s error=%s",
                                    entry.path.c_str(),
                                    probe_error.c_str());
                            }
                            if (!database.update_media_metadata(enriched)) {
                                write_ok = false;
                                return false;
                            }
                        }
                    }
                    return true;
                },
                limits,
                [&]() {
                    SDL_LockMutex(self->mutex);
                    const bool value = self->cancel_scan;
                    SDL_UnlockMutex(self->mutex);
                    return value;
                });
            SDL_LockMutex(self->mutex);
            const bool cancelled_after_scan = self->cancel_scan;
            SDL_UnlockMutex(self->mutex);
            if (database_scan) {
                database.finish_scan(
                    result.fully_enumerated() && write_ok &&
                    !cancelled_after_scan &&
                    fatal_probe_errno == 0);
            }
            all_scans_complete =
                all_scans_complete && result.fully_enumerated() &&
                !cancelled_after_scan && fatal_probe_errno == 0;
            if (fatal_probe_errno != 0) {
                diagnostics_log(
                    DiagnosticLevel::error,
                    "library-metadata fatal path=%s errno=%d",
                    fatal_probe_path.c_str(),
                    fatal_probe_errno);
                break;
            }
            if (!result.fully_enumerated() && !cancelled_after_scan) {
                diagnostics_log(
                    DiagnosticLevel::warning,
                    "library-scan incomplete cached-rows-retained root=%s entries=%zu media=%zu recoverable=%zu first_errno=%d first_path=%s",
                    root.c_str(),
                    result.entries_seen,
                    result.media_items,
                    result.recoverable_errors,
                    result.first_recoverable_errno,
                    result.first_recoverable_path.c_str());
            }
            if (!result.ok()) {
                diagnostics_log(
                    DiagnosticLevel::error,
                    "library-scan aborted root=%s path=%s errno=%d",
                    root.c_str(),
                    result.fatal_path.c_str(),
                    result.fatal_errno);
                break;
            }
            diagnostics_log(
                DiagnosticLevel::info,
                "library-scan end root=%s complete=%d entries=%zu media=%zu recoverable=%zu first_errno=%d first_path=%s skipped_symlinks=%zu skipped_devices=%zu",
                root.c_str(),
                result.fully_enumerated() ? 1 : 0,
                result.entries_seen,
                result.media_items,
                result.recoverable_errors,
                result.first_recoverable_errno,
                result.first_recoverable_path.c_str(),
                result.skipped_symlinks,
                result.skipped_devices);
        }

        for (const MediaSource& source : source_snapshot) {
            if (source.kind != MediaSourceKind::movie_file ||
                fatal_probe_errno != 0) {
                continue;
            }
            SDL_LockMutex(self->mutex);
            const bool cancelled = self->cancel_scan;
            SDL_UnlockMutex(self->mutex);
            if (cancelled) {
                all_scans_complete = false;
                break;
            }

            bool database_scan = database_ready && database.begin_scan(source.path);
            struct stat status {};
            const bool file_ready =
                lstat(source.path.c_str(), &status) == 0 &&
                S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode);
            const MediaKind kind = file_ready
                ? classify_media_path(source.path)
                : MediaKind::unknown;
            bool write_ok = true;
            if (file_ready && kind != MediaKind::unknown &&
                kind != MediaKind::subtitle) {
                MediaEntry entry{};
                entry.path = source.path;
                entry.name = media_source_default_title(source.path);
                const std::size_t slash = source.path.find_last_of('/');
                if (slash != std::string::npos && slash + 1U < source.path.size()) {
                    entry.name = source.path.substr(slash + 1U);
                }
                entry.kind = kind;
                entry.size = status.st_size > 0
                    ? static_cast<std::uint64_t>(status.st_size)
                    : 0;
                entry.modified_unix = static_cast<std::int64_t>(status.st_mtime);
                fallback_entries.push_back(entry);
                if (database_scan) {
                    const bool should_probe =
                        metadata_probes_remaining > 0 &&
                        metadata_probe_failures < kMetadataFailureBudget &&
                        (kind == MediaKind::video || kind == MediaKind::audio) &&
                        database.media_needs_metadata_probe(entry);
                    write_ok = database.upsert_media(entry);
                    if (write_ok && should_probe) {
                        --metadata_probes_remaining;
                        MediaEntry enriched = entry;
                        std::string probe_error;
                        int probe_io_error = 0;
                        if (!probe_media_metadata(
                                enriched,
                                probe_error,
                                &probe_io_error)) {
                            if (probe_io_error != 0) {
                                fatal_probe_errno = probe_io_error;
                                fatal_probe_path = entry.path;
                                write_ok = false;
                            } else {
                                ++metadata_probe_failures;
                                enriched.container = "unreadable";
                            }
                        }
                        if (write_ok &&
                            !database.update_media_metadata(enriched)) {
                            write_ok = false;
                        }
                    }
                }
            }
            if (database_scan) {
                database.finish_scan(write_ok);
            }
            if (!file_ready) {
                diagnostics_log(
                    DiagnosticLevel::warning,
                    "movie-source unavailable path=%s errno=%d",
                    source.path.c_str(),
                    errno);
            }
            all_scans_complete = all_scans_complete && write_ok;
        }

        if (database_ready) {
            self->persist_pending_favorites(database);
            self->persist_sort_setting(database);
        }
        std::vector<MediaEntry> final_entries =
            database_ready ? database.list_media() : std::move(fallback_entries);
        if (database_ready && !all_scans_complete) {
            // Keep the transaction-safe last-good cache, but do not hide media
            // that was successfully enumerated before a recoverable USB error.
            // These rows remain transient until a complete scan can commit.
            std::unordered_set<std::string> visible_paths;
            visible_paths.reserve(final_entries.size() + fallback_entries.size());
            for (const MediaEntry& cached : final_entries) {
                visible_paths.insert(cached.path);
            }
            for (MediaEntry& discovered : fallback_entries) {
                if (visible_paths.insert(discovered.path).second) {
                    final_entries.push_back(std::move(discovered));
                }
            }
        } else if (!database_ready && !all_scans_complete) {
            SDL_LockMutex(self->mutex);
            final_entries = self->entries;
            SDL_UnlockMutex(self->mutex);
        }
        retain_configured_media(final_entries, source_snapshot);
        annotate_media_sources(final_entries, source_snapshot);
        std::unordered_set<std::string> series_groups;
        std::size_t series_episodes = 0;
        std::size_t explicit_movies = 0;
        for (const MediaEntry& entry : final_entries) {
            const std::string group = series_group_key(entry);
            if (!group.empty()) {
                series_groups.insert(group);
                ++series_episodes;
            }
            if (entry.explicit_movie) {
                ++explicit_movies;
            }
        }
        diagnostics_log(
            DiagnosticLevel::info,
            "library-grouping shows=%zu episodes=%zu explicit_movies=%zu total=%zu",
            series_groups.size(),
            series_episodes,
            explicit_movies,
            final_entries.size());
        self->publish(std::move(final_entries));
        diagnostics_log(
            DiagnosticLevel::info,
            "library-scan publish complete=%d database=%d",
            all_scans_complete ? 1 : 0,
            database_ready ? 1 : 0);
        return 0;
    }

    bool start_scan() {
        SDL_LockMutex(mutex);
        if (scanning) {
            SDL_UnlockMutex(mutex);
            return true;
        }
        cancel_scan = false;
        scanning = true;
        ++published_generation;
        SDL_UnlockMutex(mutex);

        if (scan_thread) {
            SDL_WaitThread(scan_thread, nullptr);
            scan_thread = nullptr;
        }
        scan_thread = SDL_CreateThread(scan_entry, "ps5mc-library-scan", this);
        if (!scan_thread) {
            SDL_LockMutex(mutex);
            scanning = false;
            SDL_UnlockMutex(mutex);
            last_error = std::string("SDL_CreateThread: ") + SDL_GetError();
            return false;
        }
        return true;
    }

    void load_cache() {
        LibraryDatabase database;
        if (database.open(database_path)) {
            std::string encoded_sources;
            if (database.get_setting(
                    "library.media_sources.v1",
                    encoded_sources)) {
                sources = parse_media_sources(encoded_sources);
            }
            if (!sort_setting_loaded) {
                std::string stored_sort_mode;
                if (database.get_setting("library.sort_mode", stored_sort_mode)) {
                    const std::optional<LibrarySortMode> parsed =
                        parse_library_sort_mode(stored_sort_mode);
                    if (parsed.has_value()) {
                        sort_mode = *parsed;
                    }
                }
                sort_setting_loaded = true;
            }
            entries = database.list_media();
            retain_configured_media(entries, sources);
            annotate_media_sources(entries, sources);
            overlay_pending_favorites_locked(entries);
            ++published_generation;
        }
    }

    void rebuild_labels() {
        clear_labels();
        SDL_RenderSetLogicalSize(renderer, kUiWidth, kUiHeight);
        const SDL_Color white{240, 244, 255, 255};
        const SDL_Color muted{157, 170, 196, 255};
        const auto add_footer_hint = [&](FooterGlyph glyph,
                                         const char* button,
                                         const char* action) {
            FooterHint hint{};
            hint.glyph = glyph;
            if (glyph == FooterGlyph::text_button && button && button[0]) {
                hint.button_label = make_label(
                    renderer, footer_font, button, white);
            }
            hint.action_label = make_label(
                renderer, footer_font, action ? action : "", muted);
            footer_hints.push_back(std::move(hint));
        };

        SDL_LockMutex(mutex);
        const bool browsing = browser_mode;
        SDL_UnlockMutex(mutex);
        if (browsing) {
            clear_artwork();
            std::vector<BrowserEntry> visible_browser;
            std::string current_path;
            std::string browser_notice;
            std::string progress_path;
            bool selected_is_directory = false;
            bool is_importing = false;
            std::size_t checked = 0;
            std::size_t movies_found = 0;
            std::size_t shows_found = 0;
            int total = 0;
            SDL_LockMutex(mutex);
            const std::uint64_t now = SDL_GetTicks64();
            if (!notice.empty() && now >= notice_until) {
                notice.clear();
                ++notice_generation;
            }
            total = static_cast<int>(browser_entries.size());
            selected = std::clamp(selected, 0, std::max(0, total - 1));
            if (selected < first_visible) {
                first_visible = selected;
            }
            if (selected >= first_visible + kVisibleRows) {
                first_visible = selected - kVisibleRows + 1;
            }
            first_visible = std::clamp(
                first_visible, 0, std::max(0, total - kVisibleRows));
            const int end = std::min(total, first_visible + kVisibleRows);
            for (int index = first_visible; index < end; ++index) {
                visible_browser.push_back(
                    browser_entries[static_cast<std::size_t>(index)]);
            }
            current_path = browser_path;
            browser_notice = notice;
            progress_path = import_progress_path;
            is_importing = import_running;
            checked = import_entries_checked;
            movies_found = import_movies_found;
            shows_found = import_shows_found;
            selected_is_directory =
                selected >= 0 && selected < total &&
                browser_entries[static_cast<std::size_t>(selected)].directory;
            rendered_generation = published_generation;
            rendered_selected = selected;
            rendered_first = first_visible;
            rendered_filter = filter;
            rendered_sort_mode = static_cast<int>(sort_mode);
            rendered_search_query = search_query;
            rendered_search_edit = search_edit;
            rendered_search_editing = search_editing;
            rendered_notice_generation = notice_generation;
            rendered_overlay = static_cast<int>(LibraryOverlay::none);
            rendered_overlay_selected = 0;
            SDL_UnlockMutex(mutex);

            title_label = make_label(renderer, title_font, "Add Media Source", white);
            std::string browser_status;
            if (is_importing) {
                browser_status =
                    "IMPORTING  |  " + std::to_string(checked) +
                    " CHECKED  |  " + std::to_string(movies_found) +
                    " MOVIES  |  " + std::to_string(shows_found) +
                    " TV SHOWS";
            } else {
                browser_status =
                    current_path + "  |  " + std::to_string(total) + " ITEMS";
            }
            status_label = make_label(
                renderer,
                row_font,
                fit_text_to_width(
                    row_font,
                    std::move(browser_status),
                    kUiWidth - 112),
                muted);
            if (!browser_notice.empty()) {
                notice_label = make_label(
                    renderer,
                    row_font,
                    fit_text_to_width(
                        row_font,
                        std::move(browser_notice),
                        kUiWidth - 180),
                    white);
            }
            add_footer_hint(
                FooterGlyph::cross,
                "",
                is_importing ? "Browse" : "Open / Add Movie");
            if (!is_importing) {
                add_footer_hint(FooterGlyph::triangle, "", "Add TV Folder");
            }
            add_footer_hint(
                FooterGlyph::square,
                "",
                is_importing
                    ? "Cancel Import"
                    : selected_is_directory
                    ? "Import Selected Folder"
                    : "Import Current Folder");
            add_footer_hint(FooterGlyph::circle, "", "Up");
            add_footer_hint(FooterGlyph::options, "", "Close");
            artwork_label = make_label(
                renderer,
                row_font,
                fit_text_to_width(
                    row_font,
                    is_importing && !progress_path.empty()
                        ? "CHECKING  " + progress_path
                        : "SELECT A SOURCE ON THE LEFT",
                    kArtworkPanelWidth - 28),
                muted);
            const std::vector<std::string> browser_help = is_importing
                ? std::vector<std::string>{
                      "IMPORT IN PROGRESS",
                      "Loose video files become Movies",
                      "Child folders become TV Shows",
                      "Square cancels safely",
                      "Saves only after import completes",
                  }
                : std::vector<std::string>{
                      "SELECTED ITEM",
                      "Cross      Open folder / add movie",
                      "Triangle   Add as one TV show",
                      "Square     Import as mixed library",
                      "Whole-library import is opt-in",
                  };
            browser_help_labels.reserve(browser_help.size());
            for (std::size_t index = 0; index < browser_help.size(); ++index) {
                browser_help_labels.push_back(make_label(
                    renderer,
                    row_font,
                    fit_text_to_width(
                        row_font,
                        browser_help[index],
                        kArtworkPanelWidth - 88),
                    index == 0 ? white : muted));
            }
            for (const BrowserEntry& item : visible_browser) {
                std::string text = item.directory
                    ? "FOLDER   " + item.name
                    : "MOVIE    " + item.name;
                row_labels.push_back(make_label(
                    renderer,
                    row_font,
                    fit_text_to_width(row_font, std::move(text), kListWidth - 52),
                    white));
            }
            return;
        }

        LibraryOverlay active_overlay = LibraryOverlay::none;
        int active_overlay_selected = 0;
        PlayerSettings active_player_settings{};
        bool active_search_query = false;
        SDL_LockMutex(mutex);
        active_overlay = overlay;
        active_overlay_selected = overlay_selected;
        active_player_settings = player_settings;
        active_search_query = !search_query.empty();
        SDL_UnlockMutex(mutex);
        if (active_overlay != LibraryOverlay::none) {
            clear_artwork();
            std::vector<std::string> rows;
            std::string title;
            std::string subtitle;
            switch (active_overlay) {
                case LibraryOverlay::menu:
                    title = "Media Center";
                    subtitle = "Choose what you want to do";
                    rows = {
                        "Add media",
                        active_search_query ? "Clear search" : "Search library",
                        "Controls & shortcuts",
                        "Playback settings",
                        "About & diagnostics",
                        "Exit Media Center",
                    };
                    add_footer_hint(FooterGlyph::cross, "", "Select");
                    add_footer_hint(FooterGlyph::circle, "", "Close");
                    break;
                case LibraryOverlay::settings:
                    title = "Playback settings";
                    subtitle =
                        active_overlay_selected == 3
                            ? "Sets how long temporary seek, pause, volume, and status messages remain visible"
                            : "Changes are saved automatically";
                    rows = {
                        "Default volume                                      " +
                            std::to_string(active_player_settings.volume_percent) +
                            "%",
                        "Short seek step                                    " +
                            std::to_string(active_player_settings.short_seek_seconds) +
                            " seconds",
                        "Long seek step                                     " +
                            std::to_string(active_player_settings.long_seek_seconds) +
                            " seconds",
                        "Pop-up message duration                            " +
                            std::to_string(
                                active_player_settings.osd_duration_ms / 1000) +
                            " seconds",
                        std::string("Resume where I stopped                              ") +
                            (active_player_settings.resume_playback ? "On" : "Off"),
                        std::string("Automatically select subtitles                     ") +
                            (active_player_settings.auto_subtitles ? "On" : "Off"),
                        "Restore playback defaults",
                    };
                    add_footer_hint(FooterGlyph::dpad, "", "Change");
                    add_footer_hint(FooterGlyph::cross, "", "Change");
                    add_footer_hint(FooterGlyph::square, "", "Reset All");
                    add_footer_hint(FooterGlyph::circle, "", "Back");
                    break;
                case LibraryOverlay::controls:
                    title = "Controls";
                    subtitle = "Library first, playback second";
                    rows = {
                        "Cross        Open or play                         Play / pause",
                        "Circle       Queue or go back                     Subtitle track",
                        "Square       Add Media: Import folder             Audio track",
                        "Triangle     Add TV / remove source               Video track",
                        "D-pad        Navigate and change category         Seek short / long",
                        "L1 / R1      Previous or next page                 Previous / next chapter",
                        "L3 / R3      Favorite / sort                      Volume down / up",
                        "Touchpad     Add media                             Full controls",
                        "L2/R2 + Triangle                                  Crop / aspect / scale",
                        "Options      Add, search, settings, exit           Playback menu",
                    };
                    add_footer_hint(FooterGlyph::circle, "", "Back");
                    break;
                case LibraryOverlay::about:
                    title = "About PS5 Media Center";
                    subtitle = "Private test software for your jailbroken PS5";
                    rows = {
                        "Standalone native PS5 payload",
                        "FFmpeg 7.0.1 with SDL_kitchensink and SDL2",
                        "Library and logs: /data/PS5-MediaCenter",
                        "Dashboard title: PSMC00001 (Media)",
                        "Build: " PS5MC_VERSION,
                    };
                    add_footer_hint(FooterGlyph::circle, "", "Back");
                    break;
                case LibraryOverlay::none:
                    break;
            }
            title_label = make_label(renderer, title_font, title, white);
            status_label = make_label(renderer, row_font, subtitle, muted);
            std::string active_notice;
            SDL_LockMutex(mutex);
            if (!notice.empty() && SDL_GetTicks64() >= notice_until) {
                notice.clear();
                ++notice_generation;
            }
            active_notice = notice;
            rendered_notice_generation = notice_generation;
            SDL_UnlockMutex(mutex);
            if (!active_notice.empty()) {
                notice_label = make_label(
                    renderer,
                    row_font,
                    fit_text_to_width(
                        row_font,
                        std::move(active_notice),
                        kUiWidth - 180),
                    white);
            }
            for (std::string& row : rows) {
                row_labels.push_back(make_label(
                    renderer,
                    row_font,
                    fit_text_to_width(row_font, std::move(row), kUiWidth - 180),
                    white));
            }
            rendered_generation = published_generation;
            rendered_overlay = static_cast<int>(active_overlay);
            rendered_overlay_selected = active_overlay_selected;
            rendered_selected = selected;
            rendered_first = first_visible;
            rendered_notice_generation = notice_generation;
            return;
        }

        std::string series_root;
        std::string season_root;
        SDL_LockMutex(mutex);
        series_root = active_series_root;
        season_root = active_season_root;
        SDL_UnlockMutex(mutex);
        title_label = make_label(
            renderer,
            title_font,
            !season_root.empty()
                ? media_source_default_title(season_root)
                : (series_root.empty()
                       ? "Library"
                       : media_source_default_title(series_root)),
            white);
        std::vector<MediaEntry> visible;
        std::vector<int> visible_episode_counts;
        std::vector<std::string> visible_season_roots;
        bool is_scanning = false;
        bool library_has_entries = false;
        bool is_search_editing = false;
        int total = 0;
        int active_filter = 0;
        LibrarySortMode active_sort_mode = LibrarySortMode::smart;
        std::string active_query;
        std::string active_search_edit;
        std::string active_notice;
        std::string active_media_path;
        int active_generation = 0;
        SDL_LockMutex(mutex);
        const std::uint64_t now = SDL_GetTicks64();
        if (!notice.empty() && now >= notice_until) {
            notice.clear();
            ++notice_generation;
        }
        const std::vector<std::size_t>& filtered = filtered_indices_locked();
        total = static_cast<int>(filtered.size());
        selected = std::clamp(selected, 0, std::max(0, total - 1));
        if (selected < first_visible) {
            first_visible = selected;
        }
        if (selected >= first_visible + kVisibleRows) {
            first_visible = selected - kVisibleRows + 1;
        }
        first_visible = std::max(0, std::min(first_visible, std::max(0, total - kVisibleRows)));
        const int end = std::min(total, first_visible + kVisibleRows);
        for (int index = first_visible; index < end; ++index) {
            const MediaEntry& entry =
                entries[filtered[static_cast<std::size_t>(index)]];
            visible.push_back(entry);
            const std::string group = series_group_key(entry);
            const std::string season =
                series_root.empty()
                    ? std::string{}
                    : media_season_root(entry, series_root);
            int episode_count = 0;
            if (active_series_root.empty() && !group.empty()) {
                episode_count = static_cast<int>(std::count_if(
                    entries.begin(),
                    entries.end(),
                    [&](const MediaEntry& candidate) {
                        return series_group_key(candidate) == group;
                    }));
            } else if (!series_root.empty() && season_root.empty() &&
                       !season.empty()) {
                episode_count = static_cast<int>(std::count_if(
                    entries.begin(),
                    entries.end(),
                    [&](const MediaEntry& candidate) {
                        return series_group_key(candidate) == series_root &&
                            media_season_root(candidate, series_root) ==
                                season;
                    }));
            }
            visible_episode_counts.push_back(episode_count);
            visible_season_roots.push_back(season);
        }
        is_scanning = scanning;
        library_has_entries = !entries.empty();
        is_search_editing = search_editing;
        active_filter = filter;
        active_sort_mode = sort_mode;
        active_query = search_query;
        active_search_edit = search_edit;
        active_notice = notice;
        if (total > 0) {
            active_media_path =
                entries[filtered[static_cast<std::size_t>(selected)]].path;
        }
        active_generation = published_generation;
        rendered_generation = published_generation;
        rendered_filter = filter;
        rendered_sort_mode = static_cast<int>(sort_mode);
        rendered_search_query = search_query;
        rendered_search_edit = search_edit;
        rendered_search_editing = search_editing;
        rendered_notice_generation = notice_generation;
        rendered_overlay = static_cast<int>(LibraryOverlay::none);
        rendered_overlay_selected = 0;
        SDL_UnlockMutex(mutex);

        if (total == 0 && !library_has_entries) {
            add_footer_hint(FooterGlyph::cross, "", "Add Media");
            add_footer_hint(FooterGlyph::options, "", "Menu");
        } else if (total == 0) {
            if (!active_series_root.empty()) {
                add_footer_hint(FooterGlyph::circle, "", "Back");
            }
            add_footer_hint(FooterGlyph::dpad, "", "Change Category");
            add_footer_hint(FooterGlyph::touchpad, "", "Add Media");
            add_footer_hint(FooterGlyph::options, "", "Menu");
        } else if (series_root.empty()) {
            add_footer_hint(FooterGlyph::cross, "", "Play");
            add_footer_hint(
                FooterGlyph::text_button,
                "R3",
                "Sort");
            add_footer_hint(FooterGlyph::touchpad, "", "Add Media");
            add_footer_hint(FooterGlyph::options, "", "Menu");
        } else {
            add_footer_hint(
                FooterGlyph::cross,
                "",
                season_root.empty() ? "Open / Play" : "Play Episode");
            add_footer_hint(FooterGlyph::circle, "", "Back");
            add_footer_hint(FooterGlyph::touchpad, "", "Add Media");
            add_footer_hint(FooterGlyph::options, "", "Menu");
        }

        update_artwork(active_media_path, active_generation);
        bool is_thumbnail_loading = false;
        SDL_LockMutex(mutex);
        is_thumbnail_loading = thumbnail_loading;
        SDL_UnlockMutex(mutex);

        std::string status =
            (active_series_root.empty()
                 ? std::string(filter_name(active_filter))
                 : (season_root.empty()
                        ? "SEASONS & EPISODES"
                        : "EPISODES")) +
            "  |  " +
            std::to_string(total) + " ITEMS  |  SORT: " +
            (active_series_root.empty()
                 ? library_sort_mode_name(active_sort_mode)
                 : "EPISODE ORDER");
        if (!active_query.empty()) {
            status += "  |  SEARCH: " + active_query;
        }
        if (is_search_editing) {
            status += "  |  ENTER SEARCH: " + active_search_edit;
        }
        if (is_scanning) {
            status += "  |  INDEXING SELECTED SOURCES...";
        }
        status_label = make_label(
            renderer,
            row_font,
            fit_text_to_width(row_font, std::move(status), kUiWidth - 220),
            muted);
        if (!active_notice.empty()) {
            notice_label = make_label(
                renderer,
                row_font,
                fit_text_to_width(
                    row_font,
                    std::move(active_notice),
                    kUiWidth - 180),
                white);
        }
        if (total == 0) {
            const bool first_run = !library_has_entries && !is_scanning;
            std::string empty_title;
            std::string empty_help;
            if (is_scanning) {
                empty_title = "Building your library";
                empty_help = "Indexing the media sources you selected...";
            } else if (first_run) {
                empty_title = "Your library is empty";
                empty_help =
                    "Press Cross to choose a movie or TV-show folder.";
            } else if (!active_query.empty()) {
                empty_title = "No search results";
                empty_help =
                    "Open Options to clear the search, or change category.";
            } else if (!active_series_root.empty()) {
                empty_title = "No episodes found";
                empty_help = "Press Circle to return to the library.";
            } else {
                empty_title = "No items in this category";
                empty_help =
                    "Use D-pad Left or Right to change category.";
            }
            empty_action_enabled = first_run;
            empty_title_label = make_label(
                renderer,
                title_font,
                empty_title,
                white);
            empty_help_label = make_label(
                renderer,
                row_font,
                empty_help,
                muted);
        }
        std::string artwork_status;
        if (artwork_texture && artwork_is_video_frame) {
            artwork_status =
                "VIDEO PREVIEW  |  " + format_duration(artwork_position_ms) +
                "  |  " + std::to_string(artwork_width) + "x" +
                std::to_string(artwork_height);
        } else if (artwork_texture) {
            artwork_status =
                "LOCAL ARTWORK  |  " + std::to_string(artwork_width) + "x" +
                std::to_string(artwork_height);
        } else if (is_thumbnail_loading) {
            artwork_status = "GENERATING VIDEO PREVIEW...";
        } else {
            artwork_status = "NO PREVIEW AVAILABLE";
        }
        artwork_label = make_label(
            renderer,
            row_font,
            fit_text_to_width(row_font, artwork_status, kArtworkPanelWidth - 28),
            muted);
        row_labels.reserve(visible.size());
        for (std::size_t visible_index = 0;
             visible_index < visible.size();
             ++visible_index) {
            const MediaEntry& entry = visible[visible_index];
            const std::string group = series_group_key(entry);
            const std::string& season =
                visible_season_roots[visible_index];
            std::string text;
            if (active_series_root.empty() && !group.empty()) {
                text = "TV SHOW   " + series_display_name(entry) + "   -   " +
                    std::to_string(visible_episode_counts[visible_index]) +
                    " EPISODES";
            } else if (!series_root.empty() && season_root.empty() &&
                       !season.empty()) {
                text = "SEASON    " + media_source_default_title(season) +
                    "   -   " +
                    std::to_string(visible_episode_counts[visible_index]) +
                    " EPISODES";
            } else if (!active_series_root.empty()) {
                text = "EPISODE   " + display_name(entry);
            } else {
                text = std::string(entry_kind_name(entry)) + "   " +
                    display_name(entry);
            }
            const std::string details = row_details(entry);
            if (!details.empty() &&
                (active_series_root.empty()
                     ? group.empty()
                     : !season_root.empty() || season.empty())) {
                text += "   -   " + details;
            }
            row_labels.push_back(make_label(renderer, row_font, text, white));
        }
        rendered_selected = selected;
        rendered_first = first_visible;
    }

    void stop_scan() {
        if (!mutex) {
            return;
        }
        SDL_LockMutex(mutex);
        cancel_scan = true;
        SDL_UnlockMutex(mutex);
        if (scan_thread) {
            SDL_WaitThread(scan_thread, nullptr);
            scan_thread = nullptr;
        }
    }
};

LibraryUi::LibraryUi() : impl_(std::make_unique<Impl>()) {}

LibraryUi::~LibraryUi() {
    close();
}

bool LibraryUi::open(
    SDL_Renderer* renderer,
    const std::string& font_path,
    const std::string& logo_path,
    const std::vector<MediaSource>& initial_sources) {
    close();
    impl_->renderer = renderer;
    if (!renderer || font_path.empty()) {
        impl_->last_error = "LibraryUi::open: invalid argument";
        return false;
    }
    (void)logo_path;
    if (TTF_Init() != 0) {
        impl_->last_error = std::string("TTF_Init: ") + TTF_GetError();
        return false;
    }
    impl_->title_font = TTF_OpenFont(font_path.c_str(), 48);
    impl_->row_font = TTF_OpenFont(font_path.c_str(), 27);
    impl_->footer_font = TTF_OpenFont(font_path.c_str(), 22);
    if (!impl_->title_font || !impl_->row_font || !impl_->footer_font) {
        impl_->last_error = std::string("TTF_OpenFont: ") + TTF_GetError();
        close();
        return false;
    }
    impl_->mutex = SDL_CreateMutex();
    if (!impl_->mutex) {
        impl_->last_error = std::string("SDL_CreateMutex: ") + SDL_GetError();
        close();
        return false;
    }
    if (mkdir("/data/PS5-MediaCenter", 0777) != 0 && errno != EEXIST) {
        diagnostics_log(
            DiagnosticLevel::error,
            "library-data-directory failed errno=%d",
            errno);
    }
    impl_->load_cache();
    impl_->load_player_settings();
    bool sources_changed = false;
    for (MediaSource source : initial_sources) {
        source.path = normalize_media_source_path(std::move(source.path));
        if (source.path.empty() || source.path[0] != '/') {
            continue;
        }
        struct stat status {};
        const bool valid =
            lstat(source.path.c_str(), &status) == 0 &&
            !S_ISLNK(status.st_mode) &&
            ((source.kind == MediaSourceKind::tv_folder &&
              S_ISDIR(status.st_mode)) ||
             (source.kind == MediaSourceKind::movie_file &&
              S_ISREG(status.st_mode) &&
              classify_media_path(source.path) != MediaKind::unknown &&
              classify_media_path(source.path) != MediaKind::subtitle));
        if (!valid) {
            diagnostics_log(
                DiagnosticLevel::warning,
                "initial-media-source rejected kind=%s path=%s errno=%d",
                source.kind == MediaSourceKind::tv_folder
                    ? "tv-folder"
                    : "movie-file",
                source.path.c_str(),
                errno);
            continue;
        }
        if (source.title.empty()) {
            source.title = media_source_default_title(source.path);
        }
        const auto duplicate = std::find_if(
            impl_->sources.begin(),
            impl_->sources.end(),
            [&](const MediaSource& current) {
                return current.kind == source.kind &&
                       current.path == source.path;
            });
        if (duplicate == impl_->sources.end()) {
            diagnostics_log(
                DiagnosticLevel::info,
                "initial-media-source accepted kind=%s path=%s",
                source.kind == MediaSourceKind::tv_folder
                    ? "tv-folder"
                    : "movie-file",
                source.path.c_str());
            impl_->sources.push_back(std::move(source));
            sources_changed = true;
        }
    }
    if (sources_changed) {
        annotate_media_sources(impl_->entries, impl_->sources);
        if (!impl_->persist_sources()) {
            return false;
        }
    }
    SDL_LockMutex(impl_->mutex);
    impl_->restore_selected_path_locked(impl_->preferred_selected_path);
    SDL_UnlockMutex(impl_->mutex);
    if (sources_changed && !impl_->start_scan()) {
        return false;
    }
    return true;
}

void LibraryUi::close() {
    if (!impl_) {
        return;
    }
    if (impl_->search_editing || SDL_IsTextInputActive() == SDL_TRUE) {
        SDL_StopTextInput();
    }
    impl_->search_editing = false;
    impl_->search_edit.clear();
    impl_->ime_was_visible = false;
    impl_->stop_scan();
    impl_->stop_bulk_import();
    if (impl_->mutex) {
        SDL_LockMutex(impl_->mutex);
        const bool has_pending_favorites = !impl_->pending_favorites.empty();
        SDL_UnlockMutex(impl_->mutex);
        if (has_pending_favorites) {
            LibraryDatabase database;
            if (database.open(impl_->database_path)) {
                impl_->persist_pending_favorites(database);
            }
        }
        SDL_LockMutex(impl_->mutex);
        const std::string selected_path = impl_->selected_path_locked();
        if (!selected_path.empty()) {
            impl_->preferred_selected_path = selected_path;
        }
        SDL_UnlockMutex(impl_->mutex);
    }
    impl_->clear_labels();
    impl_->clear_artwork();
    if (impl_->title_font) {
        TTF_CloseFont(impl_->title_font);
        impl_->title_font = nullptr;
    }
    if (impl_->row_font) {
        TTF_CloseFont(impl_->row_font);
        impl_->row_font = nullptr;
    }
    if (impl_->footer_font) {
        TTF_CloseFont(impl_->footer_font);
        impl_->footer_font = nullptr;
    }
    if (impl_->mutex) {
        SDL_DestroyMutex(impl_->mutex);
        impl_->mutex = nullptr;
    }
    if (TTF_WasInit()) {
        TTF_Quit();
    }
    impl_->renderer = nullptr;
    impl_->entries.clear();
    impl_->overlay = LibraryOverlay::none;
    impl_->overlay_selected = 0;
}

LibraryAction LibraryUi::handle_event(const SDL_Event& event, std::string& selected_path) {
    if (!impl_ || !impl_->mutex) {
        return LibraryAction::none;
    }

    SDL_LockMutex(impl_->mutex);
    const bool editing_search = impl_->search_editing;
    const bool browsing = impl_->browser_mode;
    const bool overlay_open = impl_->overlay != LibraryOverlay::none;
    SDL_UnlockMutex(impl_->mutex);

    if (event.type == SDL_QUIT) {
        if (editing_search) {
            impl_->finish_search(false);
        }
        return LibraryAction::exit;
    }

    if (event.type == SDL_CONTROLLERDEVICEADDED ||
        event.type == SDL_CONTROLLERDEVICEREMOVED) {
        SDL_LockMutex(impl_->mutex);
        impl_->controller_buttons_down.fill(false);
        SDL_UnlockMutex(impl_->mutex);
        diagnostics_log(
            DiagnosticLevel::info,
            "library-controller state-reset event=%s id=%d",
            event.type == SDL_CONTROLLERDEVICEADDED ? "added" : "removed",
            event.cdevice.which);
        return LibraryAction::none;
    }

    if (event.type == SDL_CONTROLLERBUTTONUP) {
        SDL_LockMutex(impl_->mutex);
        if (event.cbutton.button < impl_->controller_buttons_down.size()) {
            impl_->controller_buttons_down[event.cbutton.button] = false;
        }
        SDL_UnlockMutex(impl_->mutex);
        diagnostics_log(
            DiagnosticLevel::info,
            "library-controller button-up id=%u",
            static_cast<unsigned int>(event.cbutton.button));
        return LibraryAction::none;
    }

    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        bool duplicate_down = false;
        SDL_LockMutex(impl_->mutex);
        if (event.cbutton.button < impl_->controller_buttons_down.size()) {
            duplicate_down =
                impl_->controller_buttons_down[event.cbutton.button];
            impl_->controller_buttons_down[event.cbutton.button] = true;
        }
        SDL_UnlockMutex(impl_->mutex);
        diagnostics_log(
            duplicate_down ? DiagnosticLevel::warning : DiagnosticLevel::info,
            "library-controller button-down id=%u duplicate=%d",
            static_cast<unsigned int>(event.cbutton.button),
            duplicate_down ? 1 : 0);
        if (duplicate_down) {
            return LibraryAction::none;
        }
        if (overlay_open) {
            return impl_->handle_overlay_button(event.cbutton.button)
                ? LibraryAction::exit
                : LibraryAction::none;
        }
        if (browsing) {
            impl_->handle_browser_button(event.cbutton.button);
            return LibraryAction::none;
        }
    }

    if (browsing && event.type == SDL_KEYDOWN) {
        int button = -1;
        switch (event.key.keysym.sym) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                button = SDL_CONTROLLER_BUTTON_A;
                break;
            case SDLK_BACKSPACE:
                button = SDL_CONTROLLER_BUTTON_B;
                break;
            case SDLK_ESCAPE:
                button = kControllerOptionsButton;
                break;
            case SDLK_UP:
                button = SDL_CONTROLLER_BUTTON_DPAD_UP;
                break;
            case SDLK_DOWN:
                button = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
                break;
            case SDLK_PAGEUP:
                button = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
                break;
            case SDLK_PAGEDOWN:
                button = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
                break;
            case SDLK_x:
                button = SDL_CONTROLLER_BUTTON_X;
                break;
            case SDLK_y:
                button = SDL_CONTROLLER_BUTTON_Y;
                break;
            default:
                break;
        }
        if (button >= 0) {
            impl_->handle_browser_button(static_cast<Uint8>(button));
        }
        return LibraryAction::none;
    }

    if (overlay_open && event.type == SDL_KEYDOWN) {
        int button = -1;
        switch (event.key.keysym.sym) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                button = SDL_CONTROLLER_BUTTON_A;
                break;
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE:
                button = SDL_CONTROLLER_BUTTON_B;
                break;
            case SDLK_UP:
                button = SDL_CONTROLLER_BUTTON_DPAD_UP;
                break;
            case SDLK_DOWN:
                button = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
                break;
            case SDLK_LEFT:
                button = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
                break;
            case SDLK_RIGHT:
                button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
                break;
            case SDLK_x:
                button = SDL_CONTROLLER_BUTTON_X;
                break;
            default:
                break;
        }
        if (button >= 0) {
            return impl_->handle_overlay_button(static_cast<Uint8>(button))
                ? LibraryAction::exit
                : LibraryAction::none;
        }
        return LibraryAction::none;
    }

    if (editing_search && event.type == SDL_TEXTINPUT) {
        SDL_LockMutex(impl_->mutex);
        impl_->search_edit.append(event.text.text);
        while (impl_->search_edit.size() > kMaxSearchBytes) {
            erase_last_utf8_codepoint(impl_->search_edit);
        }
        SDL_UnlockMutex(impl_->mutex);
        return LibraryAction::none;
    }

    if (editing_search && event.type == SDL_KEYDOWN) {
        switch (event.key.keysym.sym) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                impl_->finish_search(true);
                break;
            case SDLK_ESCAPE:
                impl_->finish_search(false);
                break;
            case SDLK_BACKSPACE:
                SDL_LockMutex(impl_->mutex);
                erase_last_utf8_codepoint(impl_->search_edit);
                SDL_UnlockMutex(impl_->mutex);
                break;
            case SDLK_DELETE:
                SDL_LockMutex(impl_->mutex);
                impl_->search_edit.clear();
                SDL_UnlockMutex(impl_->mutex);
                break;
            default:
                break;
        }
        return LibraryAction::none;
    }

    // The PS5 IME owns controller input while shown. Consuming any controller
    // event that leaks through prevents an OK/cancel press from also playing.
    if (editing_search && event.type == SDL_CONTROLLERBUTTONDOWN) {
        return LibraryAction::none;
    }

    const auto open_browser_for_empty_library = [&]() {
        SDL_LockMutex(impl_->mutex);
        const bool empty = impl_->entries.empty();
        SDL_UnlockMutex(impl_->mutex);
        if (empty) {
            impl_->open_browser();
        }
        return empty;
    };

    int delta = 0;
    bool play = false;
    bool play_queue = false;
    bool leave_series = false;
    bool leave_season = false;
    bool remove_source = false;
    bool begin_search = false;
    bool clear_search = false;
    bool cycle_sort = false;
    bool toggle_favorite = false;
    int filter_delta = 0;
    bool exit = false;
    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        switch (event.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_A:
                if (open_browser_for_empty_library()) {
                    return LibraryAction::none;
                }
                play = true;
                break;
            case SDL_CONTROLLER_BUTTON_B:
                SDL_LockMutex(impl_->mutex);
                leave_season = !impl_->active_season_root.empty();
                leave_series =
                    !leave_season && !impl_->active_series_root.empty();
                SDL_UnlockMutex(impl_->mutex);
                play_queue = !leave_season && !leave_series;
                break;
            case SDL_CONTROLLER_BUTTON_X:
                break;
            case SDL_CONTROLLER_BUTTON_Y:
                remove_source = true;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                delta = -1;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                delta = 1;
                break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                delta = -kVisibleRows;
                break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                delta = kVisibleRows;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                filter_delta = -1;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                filter_delta = 1;
                break;
            case kControllerTouchpadButton:
                impl_->open_browser();
                return LibraryAction::none;
                break;
#if !defined(PS5MC_PS5)
            case SDL_CONTROLLER_BUTTON_BACK:
                clear_search = true;
                break;
#endif
            case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
                cycle_sort = true;
                break;
            case SDL_CONTROLLER_BUTTON_LEFTSTICK:
                toggle_favorite = true;
                break;
            case kControllerOptionsButton:
                impl_->open_overlay(LibraryOverlay::menu);
                return LibraryAction::none;
            default:
                break;
        }
    } else if (event.type == SDL_KEYDOWN) {
        switch (event.key.keysym.sym) {
            case SDLK_RETURN:
                if (open_browser_for_empty_library()) {
                    return LibraryAction::none;
                }
                play = true;
                break;
            case SDLK_q:
                play_queue = true;
                break;
            case SDLK_F5:
                break;
            case SDLK_DELETE:
                remove_source = true;
                break;
            case SDLK_SLASH:
                begin_search = true;
                break;
            case SDLK_c:
                clear_search = true;
                break;
            case SDLK_s:
                cycle_sort = true;
                break;
            case SDLK_f:
                toggle_favorite = true;
                break;
            case SDLK_LEFT:
                filter_delta = -1;
                break;
            case SDLK_RIGHT:
                filter_delta = 1;
                break;
            case SDLK_UP:
                delta = -1;
                break;
            case SDLK_DOWN:
                delta = 1;
                break;
            case SDLK_PAGEUP:
                delta = -kVisibleRows;
                break;
            case SDLK_PAGEDOWN:
                delta = kVisibleRows;
                break;
            case SDLK_ESCAPE:
                impl_->open_overlay(LibraryOverlay::menu);
                return LibraryAction::none;
            case SDLK_F1:
                impl_->open_overlay(LibraryOverlay::controls);
                return LibraryAction::none;
            case SDLK_F2:
                impl_->open_overlay(LibraryOverlay::settings);
                return LibraryAction::none;
            default:
                break;
        }
    }

    if (begin_search) {
        impl_->begin_search();
    }
    if (leave_season || leave_series) {
        SDL_LockMutex(impl_->mutex);
        if (leave_season) {
            impl_->active_season_root.clear();
        } else {
            impl_->active_series_root.clear();
            impl_->active_season_root.clear();
        }
        impl_->selected = 0;
        impl_->first_visible = 0;
        ++impl_->published_generation;
        SDL_UnlockMutex(impl_->mutex);
    }
    if (clear_search) {
        impl_->clear_search();
    }
    if (cycle_sort) {
        impl_->cycle_sort_mode();
    }
    if (filter_delta != 0) {
        impl_->cycle_filter(filter_delta);
    }
    if (toggle_favorite) {
        impl_->toggle_selected_favorite();
    }
    if (remove_source) {
        impl_->remove_selected_source();
    }
    if (delta != 0) {
        SDL_LockMutex(impl_->mutex);
        const std::vector<std::size_t>& filtered = impl_->filtered_indices_locked();
        impl_->selected = std::clamp(
            impl_->selected + delta, 0, std::max(0, static_cast<int>(filtered.size()) - 1));
        SDL_UnlockMutex(impl_->mutex);
    }
    if (play || play_queue) {
        SDL_LockMutex(impl_->mutex);
        const std::vector<std::size_t>& filtered = impl_->filtered_indices_locked();
        if (!filtered.empty() && impl_->selected >= 0 &&
            impl_->selected < static_cast<int>(filtered.size())) {
            const MediaEntry& entry = impl_->entries[
                filtered[static_cast<std::size_t>(impl_->selected)]];
            const std::string group = series_group_key(entry);
            if (play && impl_->active_series_root.empty() && !group.empty()) {
                impl_->active_series_root = group;
                impl_->active_season_root.clear();
                impl_->selected = 0;
                impl_->first_visible = 0;
                ++impl_->published_generation;
            } else if (
                play && !impl_->active_series_root.empty() &&
                impl_->active_season_root.empty()) {
                const std::string season = media_season_root(
                    entry, impl_->active_series_root);
                if (!season.empty()) {
                    impl_->active_season_root = season;
                    impl_->selected = 0;
                    impl_->first_visible = 0;
                    ++impl_->published_generation;
                } else {
                    selected_path = entry.path;
                }
            } else {
                selected_path = entry.path;
            }
        }
        SDL_UnlockMutex(impl_->mutex);
        if (!selected_path.empty()) {
            return play_queue ? LibraryAction::play_queue : LibraryAction::play;
        }
    }
    return exit ? LibraryAction::exit : LibraryAction::none;
}

std::vector<std::string> LibraryUi::playback_queue(
    const std::string& selected_path) const {
    std::vector<std::string> paths;
    if (!impl_ || selected_path.empty() || !impl_->mutex) {
        return paths;
    }
    SDL_LockMutex(impl_->mutex);
    const std::vector<std::size_t>& filtered = impl_->filtered_indices_locked();
    std::size_t selected_position = filtered.size();
    MediaKind selected_kind = MediaKind::unknown;
    for (std::size_t position = 0; position < filtered.size(); ++position) {
        const MediaEntry& entry = impl_->entries[filtered[position]];
        if (entry.path == selected_path) {
            selected_position = position;
            selected_kind = entry.kind;
            break;
        }
    }
    if (selected_position < filtered.size()) {
        for (std::size_t position = selected_position; position < filtered.size(); ++position) {
            const MediaEntry& entry = impl_->entries[filtered[position]];
            if (entry.kind == selected_kind) {
                paths.push_back(entry.path);
            }
        }
    }
    SDL_UnlockMutex(impl_->mutex);
    if (paths.empty()) {
        paths.push_back(selected_path);
    }
    return paths;
}

void LibraryUi::show_notice(std::string message, std::uint64_t milliseconds) {
    if (!impl_ || !impl_->mutex) {
        return;
    }
    SDL_LockMutex(impl_->mutex);
    impl_->notice = std::move(message);
    const std::uint64_t now = SDL_GetTicks64();
    impl_->notice_until = now > UINT64_MAX - milliseconds
        ? UINT64_MAX
        : now + milliseconds;
    ++impl_->notice_generation;
    SDL_UnlockMutex(impl_->mutex);
}

void LibraryUi::render() {
    impl_->update_ime_state();
    impl_->consume_thumbnail_result();
    impl_->consume_bulk_import_result();
    SDL_RenderSetLogicalSize(impl_->renderer, kUiWidth, kUiHeight);
    SDL_LockMutex(impl_->mutex);
    const bool notice_expired = !impl_->notice.empty() &&
                                SDL_GetTicks64() >= impl_->notice_until;
    const bool dirty = impl_->rendered_generation != impl_->published_generation ||
                       impl_->rendered_selected != impl_->selected ||
                       impl_->rendered_first != impl_->first_visible ||
                       impl_->rendered_overlay != static_cast<int>(impl_->overlay) ||
                       impl_->rendered_overlay_selected != impl_->overlay_selected ||
                       impl_->rendered_filter != impl_->filter ||
                       impl_->rendered_sort_mode != static_cast<int>(impl_->sort_mode) ||
                       impl_->rendered_search_query != impl_->search_query ||
                       impl_->rendered_search_edit != impl_->search_edit ||
                       impl_->rendered_search_editing != impl_->search_editing ||
                       impl_->rendered_notice_generation != impl_->notice_generation ||
                       notice_expired;
    SDL_UnlockMutex(impl_->mutex);
    if (dirty) {
        impl_->rebuild_labels();
    }

    SDL_SetRenderDrawColor(impl_->renderer, 5, 9, 19, 255);
    SDL_RenderClear(impl_->renderer);

    SDL_SetRenderDrawColor(impl_->renderer, 9, 16, 30, 255);
    SDL_Rect header{0, 0, kUiWidth, 190};
    SDL_RenderFillRect(impl_->renderer, &header);
    SDL_SetRenderDrawColor(impl_->renderer, 25, 44, 78, 255);
    SDL_Rect header_shadow{0, 190, kUiWidth, 2};
    SDL_RenderFillRect(impl_->renderer, &header_shadow);

    if (impl_->title_label.texture) {
        SDL_Rect target{58, 40, impl_->title_label.width, impl_->title_label.height};
        SDL_RenderCopy(impl_->renderer, impl_->title_label.texture, nullptr, &target);
    }
    if (impl_->status_label.texture) {
        SDL_Rect target{
            58,
            119,
            impl_->status_label.width,
            impl_->status_label.height};
        SDL_RenderCopy(impl_->renderer, impl_->status_label.texture, nullptr, &target);
    }

    const bool overlay_open = impl_->overlay != LibraryOverlay::none;
    const bool selectable_overlay =
        impl_->overlay == LibraryOverlay::menu ||
        impl_->overlay == LibraryOverlay::settings;
    const bool empty_library_view =
        !overlay_open &&
        !impl_->browser_mode &&
        impl_->row_labels.empty();
    const int active_selection =
        overlay_open
            ? (selectable_overlay ? impl_->overlay_selected : -1)
            : impl_->selected;
    const int active_first = overlay_open ? 0 : impl_->first_visible;
    const int content_width =
        overlay_open || empty_library_view
            ? kUiWidth - 84
            : kListWidth;
    SDL_SetRenderDrawColor(impl_->renderer, 9, 16, 30, 255);
    SDL_Rect list_panel{
        42,
        kRowsTop - 12,
        content_width,
        kArtworkPanelHeight + 24};
    SDL_RenderFillRect(impl_->renderer, &list_panel);
    SDL_SetRenderDrawColor(impl_->renderer, 25, 42, 71, 255);
    SDL_RenderDrawRect(impl_->renderer, &list_panel);

    for (int row = 0; row < static_cast<int>(impl_->row_labels.size()); ++row) {
        const int absolute = active_first + row;
        SDL_Rect background{
            56,
            kRowsTop + row * kRowHeight,
            content_width - 28,
            kRowHeight - 7};
        if (absolute == active_selection) {
            SDL_SetRenderDrawColor(impl_->renderer, 27, 61, 111, 255);
        } else {
            SDL_SetRenderDrawColor(
                impl_->renderer,
                row % 2 == 0 ? 13 : 11,
                row % 2 == 0 ? 23 : 20,
                row % 2 == 0 ? 41 : 37,
                255);
        }
        SDL_RenderFillRect(impl_->renderer, &background);
        if (absolute == active_selection) {
            SDL_SetRenderDrawColor(impl_->renderer, 244, 178, 42, 255);
            SDL_Rect selector{background.x, background.y, 7, background.h};
            SDL_RenderFillRect(impl_->renderer, &selector);
            SDL_SetRenderDrawColor(impl_->renderer, 68, 146, 255, 255);
            SDL_RenderDrawRect(impl_->renderer, &background);
        }
        const Label& label = impl_->row_labels[static_cast<std::size_t>(row)];
        if (label.texture) {
            SDL_Rect target{
                background.x + 28,
                background.y + (background.h - label.height) / 2,
                label.width,
                label.height};
            SDL_RenderSetClipRect(impl_->renderer, &background);
            SDL_RenderCopy(impl_->renderer, label.texture, nullptr, &target);
            SDL_RenderSetClipRect(impl_->renderer, nullptr);
        }
    }

    if (impl_->row_labels.empty()) {
        if (impl_->empty_title_label.texture) {
            SDL_Rect target{
                kUiWidth / 2 - impl_->empty_title_label.width / 2,
                486,
                impl_->empty_title_label.width,
                impl_->empty_title_label.height};
            SDL_RenderCopy(
                impl_->renderer,
                impl_->empty_title_label.texture,
                nullptr,
                &target);
        }
        if (impl_->empty_help_label.texture) {
            const int help_y = impl_->empty_action_enabled ? 574 : 590;
            SDL_Rect action{
                kUiWidth / 2 - (impl_->empty_help_label.width + 76) / 2,
                help_y,
                impl_->empty_help_label.width + 76,
                64};
            if (impl_->empty_action_enabled) {
                SDL_SetRenderDrawColor(impl_->renderer, 26, 73, 132, 255);
                SDL_RenderFillRect(impl_->renderer, &action);
                SDL_SetRenderDrawColor(impl_->renderer, 83, 164, 255, 255);
                SDL_RenderDrawRect(impl_->renderer, &action);
            }
            SDL_Rect target{
                kUiWidth / 2 - impl_->empty_help_label.width / 2,
                action.y + (action.h - impl_->empty_help_label.height) / 2,
                impl_->empty_help_label.width,
                impl_->empty_help_label.height};
            SDL_RenderCopy(
                impl_->renderer,
                impl_->empty_help_label.texture,
                nullptr,
                &target);
        }
    }

    if (!overlay_open && !empty_library_view) {
        SDL_SetRenderDrawColor(impl_->renderer, 11, 20, 37, 255);
        SDL_Rect artwork_panel{
            kArtworkPanelX,
            kRowsTop - 12,
            kArtworkPanelWidth,
            kArtworkPanelHeight + 24};
        SDL_RenderFillRect(impl_->renderer, &artwork_panel);
        SDL_SetRenderDrawColor(impl_->renderer, 25, 42, 71, 255);
        SDL_RenderDrawRect(impl_->renderer, &artwork_panel);
    }
    if (!overlay_open &&
        !empty_library_view &&
        impl_->artwork_texture &&
        impl_->artwork_width > 0 &&
        impl_->artwork_height > 0) {
        const int available_width = kArtworkPanelWidth - 48;
        const int available_height = kArtworkPanelHeight - 104;
        const double scale = std::min(
            static_cast<double>(available_width) / impl_->artwork_width,
            static_cast<double>(available_height) / impl_->artwork_height);
        const int width = std::max(1, static_cast<int>(impl_->artwork_width * scale));
        const int height = std::max(1, static_cast<int>(impl_->artwork_height * scale));
        SDL_Rect target{
            kArtworkPanelX + (kArtworkPanelWidth - width) / 2,
            kRowsTop + 8 + (available_height - height) / 2,
            width,
            height};
        SDL_SetRenderDrawColor(impl_->renderer, 3, 7, 14, 255);
        SDL_Rect frame{target.x - 8, target.y - 8, target.w + 16, target.h + 16};
        SDL_RenderFillRect(impl_->renderer, &frame);
        SDL_RenderCopy(impl_->renderer, impl_->artwork_texture, nullptr, &target);
    } else if (!overlay_open &&
               !empty_library_view &&
               !impl_->browser_mode) {
        SDL_SetRenderDrawColor(impl_->renderer, 15, 28, 51, 255);
        SDL_Rect empty_art{
            kArtworkPanelX + 34,
            kRowsTop + 22,
            kArtworkPanelWidth - 68,
            kArtworkPanelHeight - 118};
        SDL_RenderFillRect(impl_->renderer, &empty_art);
        SDL_SetRenderDrawColor(impl_->renderer, 28, 49, 82, 255);
        SDL_RenderDrawRect(impl_->renderer, &empty_art);
    }
    if (!overlay_open &&
        impl_->browser_mode &&
        !impl_->browser_help_labels.empty()) {
        const int help_x = kArtworkPanelX + 54;
        int help_y = kRowsTop + 126;
        for (const Label& label : impl_->browser_help_labels) {
            if (label.texture) {
                SDL_Rect target{
                    help_x,
                    help_y,
                    label.width,
                    label.height};
                SDL_RenderCopy(
                    impl_->renderer,
                    label.texture,
                    nullptr,
                    &target);
            }
            help_y += 82;
        }
    }
    if (!overlay_open &&
        !empty_library_view &&
        impl_->artwork_label.texture) {
        SDL_Rect target{
            kArtworkPanelX + (kArtworkPanelWidth - impl_->artwork_label.width) / 2,
            kRowsTop + kArtworkPanelHeight - impl_->artwork_label.height - 6,
            impl_->artwork_label.width,
            impl_->artwork_label.height};
        SDL_RenderCopy(impl_->renderer, impl_->artwork_label.texture, nullptr, &target);
    }

    if (impl_->notice_label.texture) {
        const int width = std::min(impl_->notice_label.width + 64, kUiWidth - 116);
        SDL_Rect toast{
            (kUiWidth - width) / 2,
            914,
            width,
            58};
        SDL_SetRenderDrawColor(impl_->renderer, 24, 72, 126, 245);
        SDL_RenderFillRect(impl_->renderer, &toast);
        SDL_SetRenderDrawColor(impl_->renderer, 83, 164, 255, 255);
        SDL_RenderDrawRect(impl_->renderer, &toast);
        SDL_Rect target{
            toast.x + (toast.w - impl_->notice_label.width) / 2,
            toast.y + (toast.h - impl_->notice_label.height) / 2,
            impl_->notice_label.width,
            impl_->notice_label.height};
        SDL_RenderCopy(
            impl_->renderer,
            impl_->notice_label.texture,
            nullptr,
            &target);
    }

    SDL_SetRenderDrawColor(impl_->renderer, 10, 18, 33, 255);
    SDL_Rect footer{0, 990, kUiWidth, 90};
    SDL_RenderFillRect(impl_->renderer, &footer);
    SDL_SetRenderDrawColor(impl_->renderer, 28, 48, 80, 255);
    SDL_Rect footer_line{0, 990, kUiWidth, 2};
    SDL_RenderFillRect(impl_->renderer, &footer_line);
    int footer_x = 48;
    const int footer_center_y = 1035;
    for (const FooterHint& hint : impl_->footer_hints) {
        const int glyph_width = footer_glyph_width(hint);
        draw_footer_glyph(
            impl_->renderer,
            hint,
            footer_x,
            footer_center_y);
        footer_x += glyph_width + 9;
        if (hint.action_label.texture) {
            SDL_Rect target{
                footer_x,
                footer_center_y - hint.action_label.height / 2,
                hint.action_label.width,
                hint.action_label.height};
            SDL_RenderCopy(
                impl_->renderer,
                hint.action_label.texture,
                nullptr,
                &target);
            footer_x += hint.action_label.width;
        }
        footer_x += 27;
    }
    SDL_RenderPresent(impl_->renderer);
}

const std::string& LibraryUi::error() const noexcept {
    return impl_->last_error;
}

} // namespace ps5mc
