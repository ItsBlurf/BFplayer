#include "ps5mc/library_database.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace ps5mc {
namespace {

constexpr int kCurrentSchemaVersion = 2;
constexpr std::size_t kMaximumDatabaseTextBytes = 1024U * 1024U;

constexpr const char* kTables = R"sql(
CREATE TABLE IF NOT EXISTS roots (
    path TEXT PRIMARY KEY NOT NULL,
    generation INTEGER NOT NULL DEFAULT 0,
    last_success_unix INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS media (
    path TEXT PRIMARY KEY NOT NULL,
    root TEXT NOT NULL,
    name TEXT NOT NULL,
    kind INTEGER NOT NULL,
    size INTEGER NOT NULL,
    modified_unix INTEGER NOT NULL,
    duration_ms INTEGER NOT NULL DEFAULT 0,
    width INTEGER NOT NULL DEFAULT 0,
    height INTEGER NOT NULL DEFAULT 0,
    container TEXT NOT NULL DEFAULT '',
    video_codec TEXT NOT NULL DEFAULT '',
    audio_codec TEXT NOT NULL DEFAULT '',
    title TEXT NOT NULL DEFAULT '',
    last_seen_generation INTEGER NOT NULL,
    FOREIGN KEY(root) REFERENCES roots(path) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS resume (
    media_path TEXT PRIMARY KEY NOT NULL,
    position_ms INTEGER NOT NULL,
    duration_ms INTEGER NOT NULL,
    last_played_unix INTEGER NOT NULL,
    completed INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS track_preferences (
    media_path TEXT PRIMARY KEY NOT NULL,
    audio_stream INTEGER NOT NULL DEFAULT -1,
    audio_language TEXT NOT NULL DEFAULT '',
    subtitle_stream INTEGER NOT NULL DEFAULT -1,
    subtitle_language TEXT NOT NULL DEFAULT '',
    external_subtitle TEXT NOT NULL DEFAULT '',
    subtitle_delay_ms INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS favorites (
    media_path TEXT PRIMARY KEY NOT NULL,
    added_unix INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY NOT NULL,
    value TEXT NOT NULL
);
)sql";

constexpr const char* kIndexes = R"sql(
CREATE INDEX IF NOT EXISTS media_root_seen_idx
    ON media(root, last_seen_generation);
CREATE INDEX IF NOT EXISTS media_name_idx ON media(name COLLATE NOCASE);
)sql";

void finalize(sqlite3_stmt*& statement) {
    if (statement) {
        sqlite3_finalize(statement);
        statement = nullptr;
    }
}

bool bind_text(sqlite3_stmt* statement, int index, const std::string& value) {
    if (!statement ||
        value.size() > kMaximumDatabaseTextBytes ||
        value.find('\0') != std::string::npos) {
        return false;
    }
    return sqlite3_bind_text(
               statement, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) ==
           SQLITE_OK;
}

bool valid_key(const std::string& value) {
    return !value.empty() && value.size() <= kMaximumDatabaseTextBytes &&
           value.find('\0') == std::string::npos;
}

bool read_text(
    sqlite3_stmt* statement,
    int column,
    std::string& value) {
    const unsigned char* text = sqlite3_column_text(statement, column);
    if (!text) {
        value.clear();
        return sqlite3_column_type(statement, column) == SQLITE_NULL;
    }
    const int bytes = sqlite3_column_bytes(statement, column);
    if (bytes < 0 ||
        static_cast<std::size_t>(bytes) > kMaximumDatabaseTextBytes ||
        std::memchr(text, '\0', static_cast<std::size_t>(bytes)) != nullptr) {
        return false;
    }
    value.assign(
        reinterpret_cast<const char*>(text),
        static_cast<std::size_t>(bytes));
    return true;
}

} // namespace

LibraryDatabase::~LibraryDatabase() {
    close();
}

void LibraryDatabase::capture_error(const char* context) {
    error_ = context ? context : "SQLite error";
    if (database_) {
        error_ += ": ";
        error_ += sqlite3_errmsg(database_);
    }
}

bool LibraryDatabase::execute(const char* sql) {
    if (!database_ || !sql) {
        error_ = "execute: invalid state";
        return false;
    }
    char* message = nullptr;
    const int result = sqlite3_exec(database_, sql, nullptr, nullptr, &message);
    if (result == SQLITE_OK) {
        return true;
    }
    error_ = message ? message : sqlite3_errmsg(database_);
    sqlite3_free(message);
    return false;
}

bool LibraryDatabase::prepare(sqlite3_stmt** statement, const char* sql) {
    if (!database_ || !statement || !sql) {
        error_ = "prepare: invalid state";
        return false;
    }
    *statement = nullptr;
    if (sqlite3_prepare_v2(database_, sql, -1, statement, nullptr) == SQLITE_OK) {
        return true;
    }
    capture_error("prepare");
    return false;
}

bool LibraryDatabase::read_user_version(int& version) {
    version = 0;
    sqlite3_stmt* statement = nullptr;
    if (!prepare(&statement, "PRAGMA user_version;")) {
        return false;
    }
    const int step = sqlite3_step(statement);
    if (step != SQLITE_ROW) {
        capture_error("read user_version");
        finalize(statement);
        return false;
    }
    version = sqlite3_column_int(statement, 0);
    finalize(statement);
    return true;
}

bool LibraryDatabase::column_exists(
    const char* table,
    const char* column,
    bool& exists) {
    exists = false;
    if (!table || !column) {
        error_ = "column_exists: invalid argument";
        return false;
    }
    const std::string sql = "PRAGMA table_info(" + std::string(table) + ");";
    sqlite3_stmt* statement = nullptr;
    if (!prepare(&statement, sql.c_str())) {
        return false;
    }
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(statement, 1);
        if (name && std::string(reinterpret_cast<const char*>(name)) == column) {
            exists = true;
            break;
        }
    }
    if (step != SQLITE_ROW && step != SQLITE_DONE) {
        capture_error("inspect table columns");
        finalize(statement);
        return false;
    }
    finalize(statement);
    return true;
}

bool LibraryDatabase::ensure_column(
    const char* table,
    const char* column,
    const char* declaration) {
    bool exists = false;
    if (!column_exists(table, column, exists)) {
        return false;
    }
    if (exists) {
        return true;
    }
    const std::string sql =
        "ALTER TABLE " + std::string(table) + " ADD COLUMN " +
        declaration + ";";
    return execute(sql.c_str());
}

bool LibraryDatabase::migrate_schema() {
    int version = 0;
    if (!read_user_version(version)) {
        return false;
    }
    if (version < 0 || version > kCurrentSchemaVersion) {
        error_ = "Library database schema is newer than this build";
        return false;
    }
    if (!execute("BEGIN IMMEDIATE;")) {
        return false;
    }

    bool success = execute(kTables);
    if (success) {
        struct ColumnMigration {
            const char* table;
            const char* column;
            const char* declaration;
        };
        static constexpr ColumnMigration kMigrations[]{
            {"roots", "generation", "generation INTEGER NOT NULL DEFAULT 0"},
            {"roots", "last_success_unix",
             "last_success_unix INTEGER NOT NULL DEFAULT 0"},
            {"media", "root", "root TEXT NOT NULL DEFAULT ''"},
            {"media", "name", "name TEXT NOT NULL DEFAULT ''"},
            {"media", "kind", "kind INTEGER NOT NULL DEFAULT 0"},
            {"media", "size", "size INTEGER NOT NULL DEFAULT 0"},
            {"media", "modified_unix",
             "modified_unix INTEGER NOT NULL DEFAULT 0"},
            {"media", "duration_ms",
             "duration_ms INTEGER NOT NULL DEFAULT 0"},
            {"media", "width", "width INTEGER NOT NULL DEFAULT 0"},
            {"media", "height", "height INTEGER NOT NULL DEFAULT 0"},
            {"media", "container", "container TEXT NOT NULL DEFAULT ''"},
            {"media", "video_codec",
             "video_codec TEXT NOT NULL DEFAULT ''"},
            {"media", "audio_codec",
             "audio_codec TEXT NOT NULL DEFAULT ''"},
            {"media", "title", "title TEXT NOT NULL DEFAULT ''"},
            {"media", "last_seen_generation",
             "last_seen_generation INTEGER NOT NULL DEFAULT 0"},
            {"resume", "position_ms",
             "position_ms INTEGER NOT NULL DEFAULT 0"},
            {"resume", "duration_ms",
             "duration_ms INTEGER NOT NULL DEFAULT 0"},
            {"resume", "last_played_unix",
             "last_played_unix INTEGER NOT NULL DEFAULT 0"},
            {"resume", "completed",
             "completed INTEGER NOT NULL DEFAULT 0"},
        };
        for (const ColumnMigration& migration : kMigrations) {
            if (!ensure_column(
                    migration.table,
                    migration.column,
                    migration.declaration)) {
                success = false;
                break;
            }
        }
    }
    if (success) {
        success = execute(kIndexes);
    }
    if (success) {
        success = execute("PRAGMA user_version=2;");
    }
    if (success) {
        success = execute("COMMIT;");
    }
    if (!success) {
        const std::string migration_error = error_;
        execute("ROLLBACK;");
        error_ = migration_error;
    }
    return success;
}

bool LibraryDatabase::open(const std::string& path) {
    close();
    error_.clear();
    if (!valid_key(path)) {
        error_ = "open: invalid database path";
        return false;
    }
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path.c_str(), &database_, flags, nullptr) != SQLITE_OK) {
        capture_error("open");
        close();
        return false;
    }
    sqlite3_busy_timeout(database_, 3000);
    sqlite3_limit(
        database_,
        SQLITE_LIMIT_LENGTH,
        static_cast<int>(kMaximumDatabaseTextBytes));
    if (!execute("PRAGMA foreign_keys=ON;") ||
        !execute("PRAGMA journal_mode=WAL;") ||
        !execute("PRAGMA synchronous=NORMAL;") ||
        !migrate_schema()) {
        close();
        return false;
    }
    return true;
}

void LibraryDatabase::reset_scan_state() {
    finalize(scan_upsert_);
    scan_root_.clear();
    scan_generation_ = 0;
    scan_active_ = false;
}

void LibraryDatabase::close() {
    if (scan_active_ && database_) {
        execute("ROLLBACK;");
    }
    reset_scan_state();
    if (database_) {
        sqlite3_close(database_);
        database_ = nullptr;
    }
}

bool LibraryDatabase::begin_scan(const std::string& root) {
    if (!database_ || scan_active_ || root.empty()) {
        error_ = "begin_scan: invalid state";
        return false;
    }
    if (!execute("BEGIN IMMEDIATE;")) {
        return false;
    }
    scan_active_ = true;
    scan_root_ = root;

    sqlite3_stmt* root_statement = nullptr;
    if (!prepare(
            &root_statement,
            "INSERT INTO roots(path,generation) VALUES(?1,1) "
            "ON CONFLICT(path) DO UPDATE SET generation=generation+1 "
            "RETURNING generation;") ||
        !bind_text(root_statement, 1, root) ||
        sqlite3_step(root_statement) != SQLITE_ROW) {
        finalize(root_statement);
        capture_error("begin_scan root generation");
        finish_scan(false);
        return false;
    }
    scan_generation_ = sqlite3_column_int64(root_statement, 0);
    finalize(root_statement);

    if (!prepare(
            &scan_upsert_,
            "INSERT INTO media(path,root,name,kind,size,modified_unix,last_seen_generation) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7) "
            "ON CONFLICT(path) DO UPDATE SET "
            "root=excluded.root,name=excluded.name,kind=excluded.kind,"
            "duration_ms=CASE WHEN media.size<>excluded.size OR "
            "media.modified_unix<>excluded.modified_unix THEN 0 ELSE media.duration_ms END,"
            "width=CASE WHEN media.size<>excluded.size OR "
            "media.modified_unix<>excluded.modified_unix THEN 0 ELSE media.width END,"
            "height=CASE WHEN media.size<>excluded.size OR "
            "media.modified_unix<>excluded.modified_unix THEN 0 ELSE media.height END,"
            "container=CASE WHEN media.size<>excluded.size OR "
            "media.modified_unix<>excluded.modified_unix THEN '' ELSE media.container END,"
            "video_codec=CASE WHEN media.size<>excluded.size OR "
            "media.modified_unix<>excluded.modified_unix THEN '' ELSE media.video_codec END,"
            "audio_codec=CASE WHEN media.size<>excluded.size OR "
            "media.modified_unix<>excluded.modified_unix THEN '' ELSE media.audio_codec END,"
            "title=CASE WHEN media.size<>excluded.size OR "
            "media.modified_unix<>excluded.modified_unix THEN '' ELSE media.title END,"
            "size=excluded.size,modified_unix=excluded.modified_unix,"
            "last_seen_generation=excluded.last_seen_generation;")) {
        finish_scan(false);
        return false;
    }
    return true;
}

bool LibraryDatabase::upsert_media(const MediaEntry& entry) {
    if (!scan_active_ || !scan_upsert_) {
        error_ = "upsert_media: no active scan";
        return false;
    }
    sqlite3_reset(scan_upsert_);
    sqlite3_clear_bindings(scan_upsert_);
    const bool bound =
        bind_text(scan_upsert_, 1, entry.path) &&
        bind_text(scan_upsert_, 2, scan_root_) &&
        bind_text(scan_upsert_, 3, entry.name) &&
        sqlite3_bind_int(scan_upsert_, 4, static_cast<int>(entry.kind)) == SQLITE_OK &&
        sqlite3_bind_int64(
            scan_upsert_, 5,
            entry.size > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())
                ? std::numeric_limits<sqlite3_int64>::max()
                : static_cast<sqlite3_int64>(entry.size)) == SQLITE_OK &&
        sqlite3_bind_int64(scan_upsert_, 6, entry.modified_unix) == SQLITE_OK &&
        sqlite3_bind_int64(scan_upsert_, 7, scan_generation_) == SQLITE_OK;
    if (!bound || sqlite3_step(scan_upsert_) != SQLITE_DONE) {
        capture_error("upsert_media");
        return false;
    }
    return true;
}

bool LibraryDatabase::media_needs_metadata_probe(const MediaEntry& entry) {
    if (!database_ || entry.path.empty()) {
        error_ = "media_needs_metadata_probe: invalid state";
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    if (!prepare(
            &statement,
            "SELECT size,modified_unix,container FROM media WHERE path=?1;") ||
        !bind_text(statement, 1, entry.path)) {
        finalize(statement);
        return false;
    }
    const int step = sqlite3_step(statement);
    bool needed = true;
    if (step == SQLITE_ROW) {
        const sqlite3_int64 stored_size = sqlite3_column_int64(statement, 0);
        const sqlite3_int64 safe_size =
            entry.size > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())
                ? std::numeric_limits<sqlite3_int64>::max()
                : static_cast<sqlite3_int64>(entry.size);
        const unsigned char* container = sqlite3_column_text(statement, 2);
        needed = stored_size != safe_size ||
                 sqlite3_column_int64(statement, 1) != entry.modified_unix ||
                 !container || container[0] == '\0';
    } else if (step != SQLITE_DONE) {
        capture_error("media_needs_metadata_probe");
    }
    finalize(statement);
    return needed;
}

bool LibraryDatabase::update_media_metadata(const MediaEntry& entry) {
    sqlite3_stmt* statement = nullptr;
    const bool success = prepare(
                             &statement,
                             "UPDATE media SET duration_ms=?2,width=?3,height=?4,"
                             "container=?5,video_codec=?6,audio_codec=?7,title=?8 "
                             "WHERE path=?1;") &&
                         bind_text(statement, 1, entry.path) &&
                         sqlite3_bind_int64(statement, 2, entry.duration_ms) == SQLITE_OK &&
                         sqlite3_bind_int(statement, 3, entry.width) == SQLITE_OK &&
                         sqlite3_bind_int(statement, 4, entry.height) == SQLITE_OK &&
                         bind_text(statement, 5, entry.container) &&
                         bind_text(statement, 6, entry.video_codec) &&
                         bind_text(statement, 7, entry.audio_codec) &&
                         bind_text(statement, 8, entry.title) &&
                         sqlite3_step(statement) == SQLITE_DONE &&
                         sqlite3_changes(database_) == 1;
    if (!success) {
        capture_error("update_media_metadata");
    }
    finalize(statement);
    return success;
}

bool LibraryDatabase::finish_scan(bool commit) {
    if (!scan_active_) {
        error_ = "finish_scan: no active scan";
        return false;
    }
    finalize(scan_upsert_);
    bool success = true;
    if (commit) {
        sqlite3_stmt* cleanup = nullptr;
        success = prepare(
                      &cleanup,
                      "DELETE FROM media WHERE root=?1 AND last_seen_generation<>?2;") &&
                  bind_text(cleanup, 1, scan_root_) &&
                  sqlite3_bind_int64(cleanup, 2, scan_generation_) == SQLITE_OK &&
                  sqlite3_step(cleanup) == SQLITE_DONE;
        finalize(cleanup);
        if (success) {
            sqlite3_stmt* root_update = nullptr;
            success = prepare(
                          &root_update,
                          "UPDATE roots SET last_success_unix=unixepoch() WHERE path=?1;") &&
                      bind_text(root_update, 1, scan_root_) &&
                      sqlite3_step(root_update) == SQLITE_DONE;
            finalize(root_update);
        }
    }
    if (!success) {
        capture_error("finish_scan cleanup");
        commit = false;
    }
    const bool transaction_ok = execute(commit ? "COMMIT;" : "ROLLBACK;");
    reset_scan_state();
    return success && transaction_ok && commit;
}

bool LibraryDatabase::remove_root(const std::string& root) {
    if (!database_ || scan_active_ || !valid_key(root)) {
        error_ = "remove_root: invalid state";
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const bool success =
        execute("BEGIN IMMEDIATE;") &&
        prepare(&statement, "DELETE FROM roots WHERE path=?1;") &&
        bind_text(statement, 1, root) &&
        sqlite3_step(statement) == SQLITE_DONE;
    finalize(statement);
    if (!success) {
        capture_error("remove_root");
        execute("ROLLBACK;");
        return false;
    }
    if (!execute("COMMIT;")) {
        execute("ROLLBACK;");
        return false;
    }
    return true;
}

bool LibraryDatabase::save_resume(const std::string& path, const ResumeState& state) {
    if (!database_ || !valid_key(path)) {
        error_ = "save_resume: invalid state";
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const bool prepared = prepare(
        &statement,
        "INSERT INTO resume(media_path,position_ms,duration_ms,last_played_unix,completed) "
        "VALUES(?1,?2,?3,?4,?5) ON CONFLICT(media_path) DO UPDATE SET "
        "position_ms=excluded.position_ms,duration_ms=excluded.duration_ms,"
        "last_played_unix=excluded.last_played_unix,completed=excluded.completed;");
    const bool success = prepared && bind_text(statement, 1, path) &&
                         sqlite3_bind_int64(statement, 2, state.position_ms) == SQLITE_OK &&
                         sqlite3_bind_int64(statement, 3, state.duration_ms) == SQLITE_OK &&
                         sqlite3_bind_int64(statement, 4, state.last_played_unix) == SQLITE_OK &&
                         sqlite3_bind_int(statement, 5, state.completed ? 1 : 0) == SQLITE_OK &&
                         sqlite3_step(statement) == SQLITE_DONE;
    if (!success) {
        capture_error("save_resume");
    }
    finalize(statement);
    return success;
}

bool LibraryDatabase::load_resume(const std::string& path, ResumeState& state) {
    if (!database_ || !valid_key(path)) {
        error_ = "load_resume: invalid state";
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    if (!prepare(
            &statement,
            "SELECT position_ms,duration_ms,last_played_unix,completed "
            "FROM resume WHERE media_path=?1;") ||
        !bind_text(statement, 1, path)) {
        finalize(statement);
        return false;
    }
    const int step = sqlite3_step(statement);
    if (step == SQLITE_ROW) {
        state.position_ms = sqlite3_column_int64(statement, 0);
        state.duration_ms = sqlite3_column_int64(statement, 1);
        state.last_played_unix = sqlite3_column_int64(statement, 2);
        state.completed = sqlite3_column_int(statement, 3) != 0;
        finalize(statement);
        return true;
    }
    if (step != SQLITE_DONE) {
        capture_error("load_resume");
    }
    finalize(statement);
    return false;
}

bool LibraryDatabase::save_track_preferences(
    const std::string& path,
    const TrackPreferences& preferences) {
    if (!database_ || !valid_key(path)) {
        error_ = "save_track_preferences: invalid state";
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const bool success = prepare(
                             &statement,
                             "INSERT INTO track_preferences("
                             "media_path,audio_stream,audio_language,subtitle_stream,"
                             "subtitle_language,external_subtitle,subtitle_delay_ms) "
                             "VALUES(?1,?2,?3,?4,?5,?6,?7) "
                             "ON CONFLICT(media_path) DO UPDATE SET "
                             "audio_stream=excluded.audio_stream,"
                             "audio_language=excluded.audio_language,"
                             "subtitle_stream=excluded.subtitle_stream,"
                             "subtitle_language=excluded.subtitle_language,"
                             "external_subtitle=excluded.external_subtitle,"
                             "subtitle_delay_ms=excluded.subtitle_delay_ms;") &&
                         bind_text(statement, 1, path) &&
                         sqlite3_bind_int(statement, 2, preferences.audio_stream) == SQLITE_OK &&
                         bind_text(statement, 3, preferences.audio_language) &&
                         sqlite3_bind_int(statement, 4, preferences.subtitle_stream) == SQLITE_OK &&
                         bind_text(statement, 5, preferences.subtitle_language) &&
                         bind_text(statement, 6, preferences.external_subtitle) &&
                         sqlite3_bind_int64(statement, 7, preferences.subtitle_delay_ms) == SQLITE_OK &&
                         sqlite3_step(statement) == SQLITE_DONE;
    if (!success) {
        capture_error("save_track_preferences");
    }
    finalize(statement);
    return success;
}

bool LibraryDatabase::load_track_preferences(
    const std::string& path,
    TrackPreferences& preferences) {
    if (!database_ || !valid_key(path)) {
        error_ = "load_track_preferences: invalid state";
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    if (!prepare(
            &statement,
            "SELECT audio_stream,audio_language,subtitle_stream,subtitle_language,"
            "external_subtitle,subtitle_delay_ms FROM track_preferences "
            "WHERE media_path=?1;") ||
        !bind_text(statement, 1, path)) {
        finalize(statement);
        return false;
    }
    const int step = sqlite3_step(statement);
    if (step == SQLITE_ROW) {
        preferences.audio_stream = sqlite3_column_int(statement, 0);
        preferences.subtitle_stream = sqlite3_column_int(statement, 2);
        preferences.subtitle_delay_ms = sqlite3_column_int64(statement, 5);
        if (!read_text(statement, 1, preferences.audio_language) ||
            !read_text(statement, 3, preferences.subtitle_language) ||
            !read_text(statement, 4, preferences.external_subtitle)) {
            error_ = "load_track_preferences: invalid text value";
            finalize(statement);
            return false;
        }
        finalize(statement);
        return true;
    }
    if (step != SQLITE_DONE) {
        capture_error("load_track_preferences");
    }
    finalize(statement);
    return false;
}

bool LibraryDatabase::set_favorite(const std::string& path, bool favorite) {
    if (!database_ || path.empty()) {
        error_ = "set_favorite: invalid state";
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const char* sql = favorite
        ? "INSERT INTO favorites(media_path,added_unix) "
          "VALUES(?1,CAST(strftime('%s','now') AS INTEGER)) "
          "ON CONFLICT(media_path) DO NOTHING;"
        : "DELETE FROM favorites WHERE media_path=?1;";
    const bool success = prepare(&statement, sql) && bind_text(statement, 1, path) &&
                         sqlite3_step(statement) == SQLITE_DONE;
    if (!success) {
        capture_error("set_favorite");
    }
    finalize(statement);
    return success;
}

bool LibraryDatabase::set_setting(const std::string& key, const std::string& value) {
    if (!database_ || !valid_key(key)) {
        error_ = "set_setting: invalid state";
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const bool success = prepare(
                             &statement,
                             "INSERT INTO settings(key,value) VALUES(?1,?2) "
                             "ON CONFLICT(key) DO UPDATE SET value=excluded.value;") &&
                         bind_text(statement, 1, key) && bind_text(statement, 2, value) &&
                         sqlite3_step(statement) == SQLITE_DONE;
    if (!success) {
        capture_error("set_setting");
    }
    finalize(statement);
    return success;
}

bool LibraryDatabase::set_settings(
    const std::vector<std::pair<std::string, std::string>>& values) {
    if (!database_ || scan_active_ || values.empty()) {
        error_ = "set_settings: invalid state";
        return false;
    }
    if (!execute("BEGIN IMMEDIATE;")) {
        return false;
    }
    for (const auto& [key, value] : values) {
        if (!set_setting(key, value)) {
            (void)execute("ROLLBACK;");
            return false;
        }
    }
    if (!execute("COMMIT;")) {
        (void)execute("ROLLBACK;");
        return false;
    }
    return true;
}

bool LibraryDatabase::get_setting(const std::string& key, std::string& value) {
    if (!database_ || !valid_key(key)) {
        error_ = "get_setting: invalid state";
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    if (!prepare(&statement, "SELECT value FROM settings WHERE key=?1;") ||
        !bind_text(statement, 1, key)) {
        finalize(statement);
        return false;
    }
    const int step = sqlite3_step(statement);
    if (step == SQLITE_ROW) {
        if (!read_text(statement, 0, value)) {
            error_ = "get_setting: invalid text value";
            finalize(statement);
            return false;
        }
        finalize(statement);
        return true;
    }
    if (step != SQLITE_DONE) {
        capture_error("get_setting");
    }
    finalize(statement);
    return false;
}

std::vector<MediaEntry> LibraryDatabase::list_media(std::size_t limit, std::size_t offset) {
    std::vector<MediaEntry> entries;
    sqlite3_stmt* statement = nullptr;
    if (!database_ || !prepare(
            &statement,
            "SELECT m.path,m.name,m.kind,m.size,m.modified_unix,m.duration_ms,"
            "m.width,m.height,m.container,m.video_codec,m.audio_codec,m.title,"
            "COALESCE(r.position_ms,0),COALESCE(r.duration_ms,0),"
            "COALESCE(r.last_played_unix,0),COALESCE(r.completed,0),"
            "CASE WHEN f.media_path IS NULL THEN 0 ELSE 1 END "
            "FROM media AS m LEFT JOIN resume AS r ON r.media_path=m.path "
            "LEFT JOIN favorites AS f ON f.media_path=m.path "
            "ORDER BY m.name COLLATE NOCASE,m.path LIMIT ?1 OFFSET ?2;")) {
        return entries;
    }
    const sqlite3_int64 safe_limit = static_cast<sqlite3_int64>(std::min<std::size_t>(
        limit, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const sqlite3_int64 safe_offset = offset > static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max())
                                          ? std::numeric_limits<sqlite3_int64>::max()
                                          : static_cast<sqlite3_int64>(offset);
    sqlite3_bind_int64(statement, 1, safe_limit);
    sqlite3_bind_int64(statement, 2, safe_offset);

    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        MediaEntry entry{};
        if (!read_text(statement, 0, entry.path) ||
            !read_text(statement, 1, entry.name) ||
            entry.path.empty() || entry.name.empty()) {
            error_ = "list_media: invalid path or name";
            entries.clear();
            step = SQLITE_CORRUPT;
            break;
        }
        entry.kind = static_cast<MediaKind>(sqlite3_column_int(statement, 2));
        const sqlite3_int64 size = sqlite3_column_int64(statement, 3);
        entry.size = size > 0 ? static_cast<std::uint64_t>(size) : 0;
        entry.modified_unix = sqlite3_column_int64(statement, 4);
        entry.duration_ms = sqlite3_column_int64(statement, 5);
        entry.width = sqlite3_column_int(statement, 6);
        entry.height = sqlite3_column_int(statement, 7);
        if (!read_text(statement, 8, entry.container) ||
            !read_text(statement, 9, entry.video_codec) ||
            !read_text(statement, 10, entry.audio_codec) ||
            !read_text(statement, 11, entry.title)) {
            error_ = "list_media: invalid metadata text";
            entries.clear();
            step = SQLITE_CORRUPT;
            break;
        }
        entry.resume_position_ms = sqlite3_column_int64(statement, 12);
        entry.resume_duration_ms = sqlite3_column_int64(statement, 13);
        entry.last_played_unix = sqlite3_column_int64(statement, 14);
        entry.completed = sqlite3_column_int(statement, 15) != 0;
        entry.favorite = sqlite3_column_int(statement, 16) != 0;
        entries.push_back(std::move(entry));
    }
    if (step != SQLITE_DONE && step != SQLITE_CORRUPT) {
        capture_error("list_media");
        entries.clear();
    }
    finalize(statement);
    return entries;
}

} // namespace ps5mc
