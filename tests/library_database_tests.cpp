#include "ps5mc/library_database.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <iostream>
#include <string>

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
    const std::filesystem::path database_path =
        std::filesystem::temp_directory_path() / "ps5mc-library-test.db";
    std::error_code filesystem_error;
    std::filesystem::remove(database_path, filesystem_error);
    std::filesystem::remove(database_path.string() + "-wal", filesystem_error);
    std::filesystem::remove(database_path.string() + "-shm", filesystem_error);

    ps5mc::LibraryDatabase unopened;
    check(!unopened.set_setting("key", "value"),
          "operations before open fail without crashing");
    check(!unopened.open(""), "empty database path is rejected");

    const std::filesystem::path legacy_path =
        std::filesystem::temp_directory_path() / "ps5mc-library-v1-test.db";
    std::filesystem::remove(legacy_path, filesystem_error);
    sqlite3* legacy = nullptr;
    check(sqlite3_open(legacy_path.string().c_str(), &legacy) == SQLITE_OK,
          "legacy database fixture opens");
    const char* legacy_schema =
        "CREATE TABLE roots(path TEXT PRIMARY KEY NOT NULL,"
        "generation INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE media(path TEXT PRIMARY KEY NOT NULL,"
        "root TEXT NOT NULL,name TEXT NOT NULL,kind INTEGER NOT NULL,"
        "size INTEGER NOT NULL,modified_unix INTEGER NOT NULL,"
        "last_seen_generation INTEGER NOT NULL);"
        "CREATE TABLE resume(media_path TEXT PRIMARY KEY NOT NULL,"
        "position_ms INTEGER NOT NULL,duration_ms INTEGER NOT NULL,"
        "last_played_unix INTEGER NOT NULL);"
        "INSERT INTO roots(path,generation) VALUES('/legacy',1);"
        "INSERT INTO media(path,root,name,kind,size,modified_unix,"
        "last_seen_generation) VALUES('/legacy/Film.mkv','/legacy',"
        "'Film.mkv',1,123,456,1);"
        "PRAGMA user_version=1;";
    char* legacy_error = nullptr;
    check(sqlite3_exec(
              legacy, legacy_schema, nullptr, nullptr, &legacy_error) == SQLITE_OK,
          "legacy schema fixture is created");
    sqlite3_free(legacy_error);
    sqlite3_close(legacy);

    ps5mc::LibraryDatabase migrated;
    check(migrated.open(legacy_path.string()), "v1 database migrates");
    const auto migrated_entries = migrated.list_media();
    check(migrated_entries.size() == 1 &&
              migrated_entries[0].path == "/legacy/Film.mkv" &&
              migrated_entries[0].container.empty() &&
              migrated_entries[0].duration_ms == 0,
          "migration preserves legacy rows and adds metadata defaults");
    check(migrated.set_setting("migration", "ok"),
          "new v2 tables work after migration");
    migrated.close();
    std::filesystem::remove(legacy_path, filesystem_error);
    std::filesystem::remove(legacy_path.string() + "-wal", filesystem_error);
    std::filesystem::remove(legacy_path.string() + "-shm", filesystem_error);

    ps5mc::LibraryDatabase database;
    check(database.open(database_path.string()), "database opens");
    check(database.begin_scan("/media"), "first scan begins");
    check(database.upsert_media({"/media/A.mkv", "A.mkv", ps5mc::MediaKind::video, 100, 10}),
          "first row upserts");
    check(database.upsert_media({"/media/B.flac", "B.flac", ps5mc::MediaKind::audio, 200, 20}),
          "second row upserts");
    check(database.finish_scan(true), "first scan commits");
    check(database.list_media().size() == 2, "two rows listed");
    check(database.remove_root("/media"), "configured root can be removed");
    check(database.list_media().empty(), "removing root cascades indexed media");
    check(database.begin_scan("/media"), "root can be re-added after removal");
    check(database.upsert_media({"/media/A.mkv", "A.mkv", ps5mc::MediaKind::video, 100, 10}),
          "re-added movie upserts");
    check(database.upsert_media({"/media/B.flac", "B.flac", ps5mc::MediaKind::audio, 200, 20}),
          "re-added audio upserts");
    check(database.finish_scan(true), "re-added root scan commits");

    ps5mc::MediaEntry metadata_entry{
        "/media/A.mkv", "A.mkv", ps5mc::MediaKind::video, 100, 10};
    check(database.media_needs_metadata_probe(metadata_entry),
          "new media needs metadata probe");
    metadata_entry.duration_ms = 90000;
    metadata_entry.width = 1920;
    metadata_entry.height = 1080;
    metadata_entry.container = "matroska,webm";
    metadata_entry.video_codec = "h264";
    metadata_entry.audio_codec = "aac";
    metadata_entry.title = "Movie A";
    check(database.update_media_metadata(metadata_entry), "metadata update saves");
    check(!database.media_needs_metadata_probe(metadata_entry),
          "unchanged enriched media skips metadata probe");
    const auto metadata_entries = database.list_media();
    bool metadata_loaded = false;
    for (const auto& entry : metadata_entries) {
        if (entry.path == "/media/A.mkv") {
            metadata_loaded = entry.duration_ms == 90000 && entry.width == 1920 &&
                              entry.height == 1080 && entry.container == "matroska,webm" &&
                              entry.video_codec == "h264" && entry.audio_codec == "aac" &&
                              entry.title == "Movie A";
        }
    }
    check(metadata_loaded, "metadata fields round-trip through listing");

    ps5mc::ResumeState resume{12345, 90000, 777, false};
    check(database.save_resume("/media/A.mkv", resume), "resume saves");
    ps5mc::ResumeState loaded{};
    check(database.load_resume("/media/A.mkv", loaded), "resume loads");
    check(loaded.position_ms == 12345 && loaded.duration_ms == 90000,
          "resume values round-trip");
    const auto resumable_entries = database.list_media();
    bool joined_resume = false;
    for (const auto& entry : resumable_entries) {
        if (entry.path == "/media/A.mkv") {
            joined_resume = entry.resume_position_ms == 12345 &&
                            entry.resume_duration_ms == 90000 &&
                            entry.last_played_unix == 777 && !entry.completed;
        }
    }
    check(joined_resume, "media listing joins persistent resume state");
    check(database.save_resume("https://example.test/live.m3u8", resume),
          "direct URL resume does not require an indexed row");

    ps5mc::TrackPreferences preferences{};
    preferences.audio_stream = 2;
    preferences.audio_language = "jpn";
    preferences.subtitle_stream = -1;
    preferences.subtitle_language = "eng";
    preferences.external_subtitle = "/media/A.en.srt";
    preferences.subtitle_delay_ms = 750;
    check(database.save_track_preferences("/media/A.mkv", preferences),
          "track preferences save");
    ps5mc::TrackPreferences loaded_preferences{};
    check(database.load_track_preferences("/media/A.mkv", loaded_preferences),
          "track preferences load");
    check(loaded_preferences.audio_stream == 2 &&
              loaded_preferences.audio_language == "jpn" &&
              loaded_preferences.subtitle_stream == -1 &&
              loaded_preferences.subtitle_language == "eng" &&
              loaded_preferences.external_subtitle == "/media/A.en.srt" &&
              loaded_preferences.subtitle_delay_ms == 750,
          "track preferences round-trip");
    check(!database.load_track_preferences("/media/missing.mkv", loaded_preferences),
          "missing track preferences report no row");

    check(database.set_setting("sort", "natural"), "setting saves");
    check(!database.set_setting("", "invalid"), "empty setting key is rejected");
    check(!database.save_resume("", resume), "empty resume path is rejected");
    check(!database.save_track_preferences("", preferences),
          "empty preference path is rejected");
    std::string setting;
    check(database.get_setting("sort", setting) && setting == "natural", "setting loads");

    check(database.set_favorite("/media/A.mkv", true), "favorite saves");
    database.close();
    check(database.open(database_path.string()), "database reopens with favorite");
    const auto favorite_entries = database.list_media();
    bool favorite_loaded = false;
    for (const auto& entry : favorite_entries) {
        if (entry.path == "/media/A.mkv") {
            favorite_loaded = entry.favorite;
        }
    }
    check(favorite_loaded, "favorite joins media listing");
    check(database.set_favorite("/media/A.mkv", false), "favorite removes");
    const auto unfavorited_entries = database.list_media();
    bool favorite_removed = false;
    for (const auto& entry : unfavorited_entries) {
        if (entry.path == "/media/A.mkv") {
            favorite_removed = !entry.favorite;
        }
    }
    check(favorite_removed, "favorite removal joins media listing");

    check(database.begin_scan("/media"), "replacement scan begins");
    check(database.upsert_media({"/media/A.mkv", "A.mkv", ps5mc::MediaKind::video, 101, 30}),
          "replacement row upserts");
    check(database.finish_scan(true), "replacement scan commits");
    const auto replaced = database.list_media();
    check(replaced.size() == 1 && replaced[0].size == 101, "stale row removed after commit");
    const ps5mc::MediaEntry changed_entry{
        "/media/A.mkv", "A.mkv", ps5mc::MediaKind::video, 101, 30};
    check(database.media_needs_metadata_probe(changed_entry),
          "changed media invalidates stale metadata");
    check(replaced[0].container.empty() && replaced[0].duration_ms == 0,
          "changed media clears stale metadata fields");

    check(database.begin_scan("/media"), "aborted scan begins");
    check(!database.finish_scan(false), "aborted scan reports no commit");
    check(database.list_media().size() == 1, "aborted scan retains last index");

    database.close();
    std::filesystem::remove(database_path, filesystem_error);
    std::filesystem::remove(database_path.string() + "-wal", filesystem_error);
    std::filesystem::remove(database_path.string() + "-shm", filesystem_error);
    if (failures == 0) {
        std::cout << "library_database_tests: PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
