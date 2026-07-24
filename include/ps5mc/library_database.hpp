#pragma once

#include "ps5mc/media.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace ps5mc {

struct ResumeState {
    std::int64_t position_ms = 0;
    std::int64_t duration_ms = 0;
    std::int64_t last_played_unix = 0;
    bool completed = false;
};

struct TrackPreferences {
    int audio_stream = -1;
    std::string audio_language;
    int subtitle_stream = -1;
    std::string subtitle_language;
    std::string external_subtitle;
    std::int64_t subtitle_delay_ms = 0;
};

class LibraryDatabase {
public:
    LibraryDatabase() = default;
    ~LibraryDatabase();

    LibraryDatabase(const LibraryDatabase&) = delete;
    LibraryDatabase& operator=(const LibraryDatabase&) = delete;

    bool open(const std::string& path);
    void close();
    [[nodiscard]] bool is_open() const noexcept { return database_ != nullptr; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    // A scan is one transaction. Rows are only removed after finish_scan(true),
    // so an I/O failure or power interruption retains the last valid index.
    bool begin_scan(const std::string& root);
    bool upsert_media(const MediaEntry& entry);
    bool media_needs_metadata_probe(const MediaEntry& entry);
    bool update_media_metadata(const MediaEntry& entry);
    bool finish_scan(bool commit);
    bool remove_root(const std::string& root);

    bool save_resume(const std::string& path, const ResumeState& state);
    bool load_resume(const std::string& path, ResumeState& state);
    bool save_track_preferences(const std::string& path, const TrackPreferences& preferences);
    bool load_track_preferences(const std::string& path, TrackPreferences& preferences);
    bool set_favorite(const std::string& path, bool favorite);
    bool set_setting(const std::string& key, const std::string& value);
    bool get_setting(const std::string& key, std::string& value);

    [[nodiscard]] std::vector<MediaEntry> list_media(
        std::size_t limit = 100000,
        std::size_t offset = 0);

private:
    bool execute(const char* sql);
    bool prepare(sqlite3_stmt** statement, const char* sql);
    bool read_user_version(int& version);
    bool column_exists(
        const char* table,
        const char* column,
        bool& exists);
    bool ensure_column(
        const char* table,
        const char* column,
        const char* declaration);
    bool migrate_schema();
    void capture_error(const char* context);
    void reset_scan_state();

    sqlite3* database_ = nullptr;
    sqlite3_stmt* scan_upsert_ = nullptr;
    std::string scan_root_;
    std::int64_t scan_generation_ = 0;
    bool scan_active_ = false;
    std::string error_;
};

} // namespace ps5mc
