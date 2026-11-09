// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog_index_manager.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

#include "waywal/we_appworkshop.h"
#include "waywal/we_compat_scanner.h"
#include "waywal/we_project.h"
#include "waywal/we_vdf.h"
#include "waywal/wwpkg.h"

namespace wwal::studio {

namespace {

void bindRecordToStmt(sqlite3_stmt *stmt, const WorkshopItemRecord &rec)
{
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(rec.published_file_id));
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(rec.owner_steam_id));
    sqlite3_bind_text(stmt, 3, rec.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, rec.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, rec.file_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, rec.preview_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(rec.file_size_bytes));
    sqlite3_bind_int64(stmt, 8, static_cast<sqlite3_int64>(rec.time_created));
    sqlite3_bind_int64(stmt, 9, static_cast<sqlite3_int64>(rec.time_updated));
    sqlite3_bind_int64(stmt, 10, static_cast<sqlite3_int64>(rec.subscriptions));
    sqlite3_bind_int64(stmt, 11, static_cast<sqlite3_int64>(rec.favorited));
    sqlite3_bind_int64(stmt, 12, static_cast<sqlite3_int64>(rec.views));
    sqlite3_bind_double(stmt, 13, static_cast<double>(rec.rating));
    sqlite3_bind_int64(stmt, 14, static_cast<sqlite3_int64>(rec.install_state));
    sqlite3_bind_int64(stmt, 15, static_cast<sqlite3_int64>(rec.storage_backend));
    sqlite3_bind_int64(stmt, 16, static_cast<sqlite3_int64>(rec.compat_tier));
    sqlite3_bind_int64(stmt, 17, static_cast<sqlite3_int64>(rec.compat_score));
    sqlite3_bind_double(stmt, 18, static_cast<double>(rec.aspect_ratio));
    sqlite3_bind_int64(stmt, 19, static_cast<sqlite3_int64>(rec.dominant_color));
    sqlite3_bind_int64(stmt, 20, static_cast<sqlite3_int64>(rec.tag_mask));
    sqlite3_bind_text(stmt, 21, rec.tags_string.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 22, rec.description.c_str(), -1, SQLITE_TRANSIENT);
}

WorkshopItemRecord readRecordFromStmt(sqlite3_stmt *stmt)
{
    WorkshopItemRecord rec;
    rec.published_file_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    rec.owner_steam_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
    const char *titleTxt = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
    rec.title = titleTxt ? titleTxt : "";
    const char *typeTxt = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
    rec.type = typeTxt ? typeTxt : "scene";
    const char *pathTxt = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
    rec.file_path = pathTxt ? pathTxt : "";
    const char *prevTxt = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
    rec.preview_path = prevTxt ? prevTxt : "";
    rec.file_size_bytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 6));
    rec.time_created = static_cast<uint64_t>(sqlite3_column_int64(stmt, 7));
    rec.time_updated = static_cast<uint64_t>(sqlite3_column_int64(stmt, 8));
    rec.subscriptions = static_cast<uint32_t>(sqlite3_column_int64(stmt, 9));
    rec.favorited = static_cast<uint32_t>(sqlite3_column_int64(stmt, 10));
    rec.views = static_cast<uint32_t>(sqlite3_column_int64(stmt, 11));
    rec.rating = static_cast<float>(sqlite3_column_double(stmt, 12));
    rec.install_state = static_cast<uint32_t>(sqlite3_column_int64(stmt, 13));
    rec.storage_backend = static_cast<uint32_t>(sqlite3_column_int64(stmt, 14));
    rec.compat_tier = static_cast<uint32_t>(sqlite3_column_int64(stmt, 15));
    rec.compat_score = static_cast<uint32_t>(sqlite3_column_int64(stmt, 16));
    rec.aspect_ratio = static_cast<float>(sqlite3_column_double(stmt, 17));
    rec.dominant_color = static_cast<uint32_t>(sqlite3_column_int64(stmt, 18));
    rec.tag_mask = static_cast<uint64_t>(sqlite3_column_int64(stmt, 19));
    const char *tagsTxt = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 20));
    rec.tags_string = tagsTxt ? tagsTxt : "";
    const char *descTxt = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 21));
    rec.description = descTxt ? descTxt : "";
    if (sqlite3_column_count(stmt) > 22) {
        rec.search_rank = sqlite3_column_double(stmt, 22);
    }
    return rec;
}

std::filesystem::path defaultDatabasePath()
{
    const char *xdgData = std::getenv("XDG_DATA_HOME");
    std::filesystem::path baseDir;
    if (xdgData && xdgData[0] != '\0') {
        baseDir = std::filesystem::path(xdgData);
    } else {
        const char *home = std::getenv("HOME");
        baseDir = (home && home[0] != '\0') ? std::filesystem::path(home) / ".local" / "share"
                                            : std::filesystem::current_path();
    }
    return baseDir / "wwal" / "catalog.db";
}

} // namespace

struct CatalogIndexManager::Impl {
    mutable std::mutex dbMutex;
};

CatalogIndexManager::CatalogIndexManager(const std::filesystem::path &dbPath)
    : m_impl(std::make_unique<Impl>())
{
    if (dbPath.empty()) {
        m_dbPath = defaultDatabasePath();
    } else {
        m_dbPath = dbPath;
    }

    std::error_code ec;
    if (m_dbPath != ":memory:") {
        std::filesystem::create_directories(m_dbPath.parent_path(), ec);
    }

    int rc = sqlite3_open_v2(m_dbPath.string().c_str(), &m_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[CatalogIndexManager] Failed to open SQLite database %s: %s\n",
                     m_dbPath.string().c_str(), sqlite3_errmsg(m_db));
        return;
    }

    initSchema();
    prepareStatements();
}

CatalogIndexManager::~CatalogIndexManager()
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    finalizeStatements();
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

bool CatalogIndexManager::isOpen() const noexcept { return m_db != nullptr; }

void CatalogIndexManager::initSchema()
{
    if (!m_db)
        return;

    // Execute SQLite3 pragmas (§5.4.1)
    sqlite3_exec(m_db, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA cache_size = -64000;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA mmap_size = 268435456;", nullptr, nullptr, nullptr);

    // Primary Metadata Table
    const char *sqlTable = R"(
        CREATE TABLE IF NOT EXISTS workshop_items (
            published_file_id   INTEGER PRIMARY KEY NOT NULL,
            owner_steam_id      INTEGER NOT NULL DEFAULT 0,
            title               TEXT NOT NULL,
            type                TEXT NOT NULL,
            file_path           TEXT NOT NULL UNIQUE,
            preview_path        TEXT,
            file_size_bytes     INTEGER NOT NULL DEFAULT 0,
            time_created        INTEGER NOT NULL DEFAULT 0,
            time_updated        INTEGER NOT NULL DEFAULT 0,
            subscriptions       INTEGER NOT NULL DEFAULT 0,
            favorited           INTEGER NOT NULL DEFAULT 0,
            views               INTEGER NOT NULL DEFAULT 0,
            rating              REAL NOT NULL DEFAULT 0.0,
            install_state       INTEGER NOT NULL DEFAULT 0,
            storage_backend     INTEGER NOT NULL DEFAULT 0,
            compat_tier         INTEGER NOT NULL DEFAULT 1,
            compat_score        INTEGER NOT NULL DEFAULT 100,
            aspect_ratio        REAL NOT NULL DEFAULT 1.7777778,
            dominant_color      INTEGER NOT NULL DEFAULT 0x2b303c,
            tag_mask            INTEGER NOT NULL DEFAULT 0,
            tags_string         TEXT NOT NULL DEFAULT '',
            description         TEXT NOT NULL DEFAULT ''
        );
    )";
    sqlite3_exec(m_db, sqlTable, nullptr, nullptr, nullptr);

    // Compound Covering Indices for Zero-Allocation 144 FPS Filtering & Sorting (§5.4.1)
    const char *sqlIndices = R"(
        CREATE INDEX IF NOT EXISTS idx_items_type_tier_subs ON workshop_items(type, compat_tier, subscriptions DESC);
        CREATE INDEX IF NOT EXISTS idx_items_type_subs      ON workshop_items(type, subscriptions DESC);
        CREATE INDEX IF NOT EXISTS idx_items_tier_subs      ON workshop_items(compat_tier, subscriptions DESC);
        CREATE INDEX IF NOT EXISTS idx_items_tag_rating     ON workshop_items(tag_mask, rating DESC, published_file_id);
        CREATE INDEX IF NOT EXISTS idx_items_time_updated   ON workshop_items(time_updated DESC);
        CREATE INDEX IF NOT EXISTS idx_items_install_state  ON workshop_items(install_state);
    )";
    sqlite3_exec(m_db, sqlIndices, nullptr, nullptr, nullptr);

    // FTS5 Full-Text Search Virtual Table (§5.4.1)
    const char *sqlFts = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS workshop_fts USING fts5 (
            title,
            tags_string,
            description,
            content='workshop_items',
            content_rowid='published_file_id',
            tokenize='unicode61 remove_diacritics 2'
        );
    )";
    sqlite3_exec(m_db, sqlFts, nullptr, nullptr, nullptr);

    // Automatic FTS5 Synchronization Triggers (Optimized for Zero-Churn Non-Text Updates) (§5.4.1)
    const char *sqlTriggers = R"(
        CREATE TRIGGER IF NOT EXISTS trg_workshop_items_ai AFTER INSERT ON workshop_items BEGIN
            INSERT INTO workshop_fts (rowid, title, tags_string, description)
            VALUES (new.published_file_id, new.title, new.tags_string, new.description);
        END;

        CREATE TRIGGER IF NOT EXISTS trg_workshop_items_ad AFTER DELETE ON workshop_items BEGIN
            INSERT INTO workshop_fts (workshop_fts, rowid, title, tags_string, description)
            VALUES ('delete', old.published_file_id, old.title, old.tags_string, old.description);
        END;

        CREATE TRIGGER IF NOT EXISTS trg_workshop_items_au 
        AFTER UPDATE OF title, tags_string, description ON workshop_items BEGIN
            INSERT INTO workshop_fts (workshop_fts, rowid, title, tags_string, description)
            VALUES ('delete', old.published_file_id, old.title, old.tags_string, old.description);
            INSERT INTO workshop_fts (rowid, title, tags_string, description)
            VALUES (new.published_file_id, new.title, new.tags_string, new.description);
        END;
    )";
    sqlite3_exec(m_db, sqlTriggers, nullptr, nullptr, nullptr);
}

void CatalogIndexManager::prepareStatements()
{
    if (!m_db)
        return;

    // Prepared statement for upsert
    const char *sqlUpsert = R"(
        INSERT INTO workshop_items (
            published_file_id, owner_steam_id, title, type, file_path, preview_path,
            file_size_bytes, time_created, time_updated, subscriptions, favorited,
            views, rating, install_state, storage_backend, compat_tier, compat_score,
            aspect_ratio, dominant_color, tag_mask, tags_string, description
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(published_file_id) DO UPDATE SET
            owner_steam_id  = excluded.owner_steam_id,
            title           = excluded.title,
            type            = excluded.type,
            file_path       = excluded.file_path,
            preview_path    = excluded.preview_path,
            file_size_bytes = excluded.file_size_bytes,
            time_created    = excluded.time_created,
            time_updated    = excluded.time_updated,
            subscriptions   = excluded.subscriptions,
            favorited       = excluded.favorited,
            views           = excluded.views,
            rating          = excluded.rating,
            install_state   = excluded.install_state,
            storage_backend = excluded.storage_backend,
            compat_tier     = excluded.compat_tier,
            compat_score    = excluded.compat_score,
            aspect_ratio    = excluded.aspect_ratio,
            dominant_color  = excluded.dominant_color,
            tag_mask        = excluded.tag_mask,
            tags_string     = excluded.tags_string,
            description     = excluded.description;
    )";
    sqlite3_prepare_v2(m_db, sqlUpsert, -1, &m_stmtUpsert, nullptr);

    // Prepared statement for delete
    const char *sqlDelete = "DELETE FROM workshop_items WHERE published_file_id = ?;";
    sqlite3_prepare_v2(m_db, sqlDelete, -1, &m_stmtDelete, nullptr);

    // Prepared statement for get by ID
    const char *sqlGetById = R"(
        SELECT published_file_id, owner_steam_id, title, type, file_path, preview_path,
               file_size_bytes, time_created, time_updated, subscriptions, favorited,
               views, rating, install_state, storage_backend, compat_tier, compat_score,
               aspect_ratio, dominant_color, tag_mask, tags_string, description
        FROM workshop_items WHERE published_file_id = ?;
    )";
    sqlite3_prepare_v2(m_db, sqlGetById, -1, &m_stmtGetById, nullptr);

    // Prepared statement for get by file path
    const char *sqlGetByPath = R"(
        SELECT published_file_id, owner_steam_id, title, type, file_path, preview_path,
               file_size_bytes, time_created, time_updated, subscriptions, favorited,
               views, rating, install_state, storage_backend, compat_tier, compat_score,
               aspect_ratio, dominant_color, tag_mask, tags_string, description
        FROM workshop_items WHERE file_path = ?;
    )";
    sqlite3_prepare_v2(m_db, sqlGetByPath, -1, &m_stmtGetByPath, nullptr);

    // Prepared statement for total item count
    const char *sqlCount = "SELECT COUNT(*) FROM workshop_items;";
    sqlite3_prepare_v2(m_db, sqlCount, -1, &m_stmtCount, nullptr);

    // Prepared statement for FTS5 prefix match with BM25 ranking (§5.4.2)
    const char *sqlFts5 = R"(
        SELECT i.published_file_id, i.owner_steam_id, i.title, i.type, i.file_path, i.preview_path,
               i.file_size_bytes, i.time_created, i.time_updated, i.subscriptions, i.favorited,
               i.views, i.rating, i.install_state, i.storage_backend, i.compat_tier, i.compat_score,
               i.aspect_ratio, i.dominant_color, i.tag_mask, i.tags_string, i.description,
               f.rank
        FROM (
            SELECT rowid, bm25(workshop_fts, 5.0, 2.0, 1.0) AS rank
            FROM workshop_fts
            WHERE workshop_fts MATCH ?
            ORDER BY rank
            LIMIT ? OFFSET ?
        ) f
        JOIN workshop_items i ON f.rowid = i.published_file_id
        ORDER BY f.rank;
    )";
    sqlite3_prepare_v2(m_db, sqlFts5, -1, &m_stmtFts5, nullptr);
}

void CatalogIndexManager::finalizeStatements()
{
    if (m_stmtUpsert) {
        sqlite3_finalize(m_stmtUpsert);
        m_stmtUpsert = nullptr;
    }
    if (m_stmtDelete) {
        sqlite3_finalize(m_stmtDelete);
        m_stmtDelete = nullptr;
    }
    if (m_stmtGetById) {
        sqlite3_finalize(m_stmtGetById);
        m_stmtGetById = nullptr;
    }
    if (m_stmtGetByPath) {
        sqlite3_finalize(m_stmtGetByPath);
        m_stmtGetByPath = nullptr;
    }
    if (m_stmtCount) {
        sqlite3_finalize(m_stmtCount);
        m_stmtCount = nullptr;
    }
    if (m_stmtFts5) {
        sqlite3_finalize(m_stmtFts5);
        m_stmtFts5 = nullptr;
    }
}

std::string CatalogIndexManager::sanitizeFts5Query(std::string_view rawQuery)
{
    if (rawQuery.empty())
        return {};

    // Strip/escape special FTS5 operators (*, ", -, :, ^, (, ), {, }, +, ~, [, ], ?, =, <, >, /, \,
    // ;, ')
    std::string cleaned;
    cleaned.reserve(rawQuery.size());
    for (char ch : rawQuery) {
        switch (ch) {
        case '*':
        case '"':
        case '-':
        case ':':
        case '^':
        case '(':
        case ')':
        case '{':
        case '}':
        case '+':
        case '~':
        case '[':
        case ']':
        case '?':
        case '=':
        case '<':
        case '>':
        case '/':
        case '\\':
        case ';':
        case '\'':
            cleaned.push_back(' ');
            break;
        default:
            cleaned.push_back(ch);
            break;
        }
    }

    // Tokenize by whitespace
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < cleaned.size()) {
        while (i < cleaned.size() && static_cast<unsigned char>(cleaned[i]) <= ' ') {
            ++i;
        }
        if (i >= cleaned.size())
            break;

        size_t start = i;
        while (i < cleaned.size() && static_cast<unsigned char>(cleaned[i]) > ' ') {
            ++i;
        }
        std::string token = cleaned.substr(start, i - start);
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }

    std::string result;
    for (size_t t = 0; t < tokens.size(); ++t) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result.push_back('"');
        result.append(tokens[t]);
        result.push_back('"');
        if (t + 1 == tokens.size()) {
            result.push_back('*');
        }
    }

    return result;
}

bool CatalogIndexManager::upsertItem(const WorkshopItemRecord &item)
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtUpsert)
        return false;

    sqlite3_reset(m_stmtUpsert);
    sqlite3_clear_bindings(m_stmtUpsert);
    bindRecordToStmt(m_stmtUpsert, item);

    int rc = sqlite3_step(m_stmtUpsert);
    sqlite3_reset(m_stmtUpsert);
    return (rc == SQLITE_DONE);
}

bool CatalogIndexManager::upsertItems(const std::vector<WorkshopItemRecord> &items)
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtUpsert)
        return false;

    sqlite3_exec(m_db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr);

    bool allOk = true;
    for (const auto &item : items) {
        sqlite3_reset(m_stmtUpsert);
        sqlite3_clear_bindings(m_stmtUpsert);
        bindRecordToStmt(m_stmtUpsert, item);
        int rc = sqlite3_step(m_stmtUpsert);
        if (rc != SQLITE_DONE) {
            allOk = false;
        }
    }

    if (allOk) {
        sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr);
    } else {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }

    sqlite3_reset(m_stmtUpsert);
    return allOk;
}

bool CatalogIndexManager::removeItem(uint64_t publishedFileId)
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtDelete)
        return false;

    sqlite3_reset(m_stmtDelete);
    sqlite3_clear_bindings(m_stmtDelete);
    sqlite3_bind_int64(m_stmtDelete, 1, static_cast<sqlite3_int64>(publishedFileId));

    int rc = sqlite3_step(m_stmtDelete);
    sqlite3_reset(m_stmtDelete);
    return (rc == SQLITE_DONE);
}

std::optional<WorkshopItemRecord> CatalogIndexManager::getItem(uint64_t publishedFileId) const
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtGetById)
        return std::nullopt;

    sqlite3_reset(m_stmtGetById);
    sqlite3_clear_bindings(m_stmtGetById);
    sqlite3_bind_int64(m_stmtGetById, 1, static_cast<sqlite3_int64>(publishedFileId));

    std::optional<WorkshopItemRecord> result;
    if (sqlite3_step(m_stmtGetById) == SQLITE_ROW) {
        result = readRecordFromStmt(m_stmtGetById);
    }
    sqlite3_reset(m_stmtGetById);
    return result;
}

std::optional<WorkshopItemRecord>
CatalogIndexManager::getItemByPath(const std::string &filePath) const
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtGetByPath)
        return std::nullopt;

    sqlite3_reset(m_stmtGetByPath);
    sqlite3_clear_bindings(m_stmtGetByPath);
    sqlite3_bind_text(m_stmtGetByPath, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<WorkshopItemRecord> result;
    if (sqlite3_step(m_stmtGetByPath) == SQLITE_ROW) {
        result = readRecordFromStmt(m_stmtGetByPath);
    }
    sqlite3_reset(m_stmtGetByPath);
    return result;
}

size_t CatalogIndexManager::totalItemCount() const
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtCount)
        return 0;

    sqlite3_reset(m_stmtCount);
    size_t count = 0;
    if (sqlite3_step(m_stmtCount) == SQLITE_ROW) {
        count = static_cast<size_t>(sqlite3_column_int64(m_stmtCount, 0));
    }
    sqlite3_reset(m_stmtCount);
    return count;
}

std::vector<WorkshopItemRecord> CatalogIndexManager::searchFts5Bm25(const std::string &query,
                                                                    int limit, int offset) const
{
    std::string sanitized = sanitizeFts5Query(query);
    if (sanitized.empty()) {
        return {};
    }

    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtFts5)
        return {};

    sqlite3_reset(m_stmtFts5);
    sqlite3_clear_bindings(m_stmtFts5);

    sqlite3_bind_text(m_stmtFts5, 1, sanitized.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_stmtFts5, 2, limit > 0 ? limit : 50);
    sqlite3_bind_int(m_stmtFts5, 3, offset >= 0 ? offset : 0);

    std::vector<WorkshopItemRecord> results;
    while (sqlite3_step(m_stmtFts5) == SQLITE_ROW) {
        results.push_back(readRecordFromStmt(m_stmtFts5));
    }

    sqlite3_reset(m_stmtFts5);
    return results;
}

std::vector<WorkshopItemRecord>
CatalogIndexManager::queryByFilters(const CatalogFilter &filter) const
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db)
        return {};

    std::ostringstream sql;
    sql << "SELECT published_file_id, owner_steam_id, title, type, file_path, preview_path, "
           "file_size_bytes, time_created, time_updated, subscriptions, favorited, "
           "views, rating, install_state, storage_backend, compat_tier, compat_score, "
           "aspect_ratio, dominant_color, tag_mask, tags_string, description "
           "FROM workshop_items";

    std::vector<std::string> conditions;
    if (!filter.type.empty() && filter.type != "all" && filter.type != "All") {
        conditions.push_back("type = ?");
    }
    if (filter.compat_tier > 0) {
        conditions.push_back("compat_tier = ?");
    }
    if (filter.required_tag_mask != 0) {
        conditions.push_back("(tag_mask & ?) = ?");
    }
    if (filter.install_state_mask != 0) {
        conditions.push_back("(install_state & ?) != 0");
    }

    if (!conditions.empty()) {
        sql << " WHERE ";
        for (size_t i = 0; i < conditions.size(); ++i) {
            if (i > 0)
                sql << " AND ";
            sql << conditions[i];
        }
    }

    // Compound Sorter (§5.4.1 covering index alignment)
    switch (filter.sort_order) {
    case CatalogSortOrder::SubscriptionsDesc:
        sql << " ORDER BY subscriptions DESC";
        break;
    case CatalogSortOrder::RatingDesc:
        sql << " ORDER BY rating DESC, published_file_id ASC";
        break;
    case CatalogSortOrder::RecentlyUpdated:
        sql << " ORDER BY time_updated DESC";
        break;
    case CatalogSortOrder::CompatibilityScoreDesc:
        sql << " ORDER BY compat_score DESC, subscriptions DESC";
        break;
    case CatalogSortOrder::TitleAsc:
        sql << " ORDER BY title COLLATE NOCASE ASC";
        break;
    case CatalogSortOrder::FileSizeDesc:
        sql << " ORDER BY file_size_bytes DESC";
        break;
    }

    sql << " LIMIT ? OFFSET ?;";

    std::string sqlStr = sql.str();
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, sqlStr.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
        return {};

    int bindIdx = 1;
    if (!filter.type.empty() && filter.type != "all" && filter.type != "All") {
        sqlite3_bind_text(stmt, bindIdx++, filter.type.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (filter.compat_tier > 0) {
        sqlite3_bind_int(stmt, bindIdx++, static_cast<int>(filter.compat_tier));
    }
    if (filter.required_tag_mask != 0) {
        sqlite3_bind_int64(stmt, bindIdx++, static_cast<sqlite3_int64>(filter.required_tag_mask));
        sqlite3_bind_int64(stmt, bindIdx++, static_cast<sqlite3_int64>(filter.required_tag_mask));
    }
    if (filter.install_state_mask != 0) {
        sqlite3_bind_int64(stmt, bindIdx++, static_cast<sqlite3_int64>(filter.install_state_mask));
    }

    int lim = filter.limit > 0 ? filter.limit : 50;
    int off = filter.offset >= 0 ? filter.offset : 0;
    sqlite3_bind_int(stmt, bindIdx++, lim);
    sqlite3_bind_int(stmt, bindIdx++, off);

    std::vector<WorkshopItemRecord> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(readRecordFromStmt(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<WorkshopItemRecord> CatalogIndexManager::queryByFilters(const std::string &type,
                                                                    uint32_t compat_tier,
                                                                    uint64_t tag_mask,
                                                                    CatalogSortOrder sort_order,
                                                                    int limit, int offset) const
{
    CatalogFilter f;
    f.type = type;
    f.compat_tier = compat_tier;
    f.required_tag_mask = tag_mask;
    f.sort_order = sort_order;
    f.limit = limit;
    f.offset = offset;
    return queryByFilters(f);
}

size_t CatalogIndexManager::reconcileAllLibraries()
{
    size_t totalIndexed = 0;

    // 1. Multi-Drive Steam Library Discovery
    waywal_steam_library_t libs[32];
    size_t libCount = waywal_steam_discover_libraries(libs, 32);

    for (size_t i = 0; i < libCount; ++i) {
        std::filesystem::path libPath(libs[i].path);
        std::filesystem::path wsDir = libPath / "steamapps" / "workshop" / "content" / "431960";
        std::error_code ec;
        if (std::filesystem::exists(wsDir, ec)) {
            totalIndexed += reconcileDirectory(wsDir, 0 /* SteamLocal */);
        }
    }

    // 2. Open Hub Packages Discovery (~/.local/share/waywal/hub and ~/.local/share/wwal/hub)
    const char *xdgData = std::getenv("XDG_DATA_HOME");
    std::filesystem::path dataRoot;
    if (xdgData && xdgData[0] != '\0') {
        dataRoot = std::filesystem::path(xdgData);
    } else {
        const char *home = std::getenv("HOME");
        dataRoot = (home && home[0] != '\0') ? std::filesystem::path(home) / ".local" / "share"
                                             : std::filesystem::current_path();
    }

    std::filesystem::path hubPath1 = dataRoot / "waywal" / "hub";
    std::filesystem::path hubPath2 = dataRoot / "wwal" / "hub";
    std::error_code ec;
    if (std::filesystem::exists(hubPath1, ec)) {
        totalIndexed += reconcileDirectory(hubPath1, 2 /* OpenHub */);
    }
    if (std::filesystem::exists(hubPath2, ec)) {
        totalIndexed += reconcileDirectory(hubPath2, 2 /* OpenHub */);
    }

    return totalIndexed;
}

size_t CatalogIndexManager::reconcileDirectory(const std::filesystem::path &workshopDir,
                                               uint32_t storageBackend)
{
    std::error_code ec;
    if (!std::filesystem::exists(workshopDir, ec) ||
        !std::filesystem::is_directory(workshopDir, ec)) {
        return 0;
    }

    size_t indexedCount = 0;

    // If Steam Workshop directory, locate and parse appworkshop_431960.vdf
    std::unordered_map<uint64_t, waywal_workshop_item_manifest_t> manifestMap;
    if (storageBackend == 0) {
        std::vector<std::filesystem::path> candidateManifests = {
            workshopDir.parent_path() / "appworkshop_431960.vdf",
            workshopDir / "appworkshop_431960.vdf",
            workshopDir.parent_path().parent_path() / "appworkshop_431960.vdf"};

        for (const auto &mPath : candidateManifests) {
            if (std::filesystem::exists(mPath, ec)) {
                waywal_appworkshop_manifest_t manifest{};
                if (waywal_appworkshop_parse_file(mPath.string().c_str(), &manifest)) {
                    for (size_t i = 0; i < manifest.item_count; ++i) {
                        manifestMap[manifest.items[i].published_file_id] = manifest.items[i];
                    }
                    waywal_appworkshop_free(&manifest);
                    break;
                }
            }
        }
    }

    // Iterate items inside workshop directory
    for (const auto &entry : std::filesystem::directory_iterator(
             workshopDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (entry.is_directory(ec)) {
            uint64_t id = 0;
            std::string folderName = entry.path().filename().string();
            try {
                id = std::stoull(folderName);
            } catch (...) {
                id = 0;
            }

            if (reconcileItemDirectory(entry.path(), id, storageBackend)) {
                indexedCount++;
            }
        } else if (storageBackend == 2 && entry.is_regular_file(ec)) {
            // Handle standalone .wwpkg archive in Open Hub directory
            std::string ext = entry.path().extension().string();
            if (ext == ".wwpkg") {
                wwpkg_package_t *pkg = wwpkg_open(entry.path().string().c_str());
                if (pkg) {
                    const wwpkg_manifest_t *man = wwpkg_get_manifest(pkg);
                    if (man) {
                        uint64_t id = 0;
                        for (size_t k = 0; k < sizeof(man->id) && man->id[k]; ++k) {
                            id = (id * 1099511628211ULL) ^ static_cast<uint8_t>(man->id[k]);
                        }
                        if (id == 0) {
                            id = std::hash<std::string>{}(man->slug);
                        }

                        WorkshopItemRecord rec;
                        rec.published_file_id = id;
                        rec.title = man->name[0] ? man->name : man->slug;
                        rec.type = (man->type == WWPKG_TYPE_VIDEO_STREAM) ? "video" : "scene";
                        rec.file_path = entry.path().string();
                        rec.file_size_bytes = entry.file_size(ec);
                        rec.storage_backend = 2; // OpenHub
                        rec.compat_tier = 1;
                        rec.compat_score = 100;
                        rec.description = man->description;

                        std::string tags;
                        for (uint32_t t = 0; t < man->tag_count; ++t) {
                            if (!tags.empty())
                                tags += ", ";
                            tags += man->tags[t];
                        }
                        rec.tags_string = tags;
                        rec.tag_mask = WallpaperTags::parseTagsFromString(tags);
                        rec.install_state = WAYWAL_UGC_STATE_INSTALLED;

                        if (upsertItem(rec)) {
                            indexedCount++;
                        }
                    }
                    wwpkg_close(pkg);
                }
            }
        }
    }

    return indexedCount;
}

bool CatalogIndexManager::reconcileItemDirectory(const std::filesystem::path &itemDir,
                                                 uint64_t publishedFileId, uint32_t storageBackend)
{
    std::error_code ec;
    if (!std::filesystem::exists(itemDir, ec) || !std::filesystem::is_directory(itemDir, ec)) {
        return false;
    }

    if (publishedFileId == 0) {
        try {
            publishedFileId = std::stoull(itemDir.filename().string());
        } catch (...) {
            publishedFileId = std::hash<std::string>{}(itemDir.filename().string());
        }
    }

    WorkshopItemRecord rec;
    rec.published_file_id = publishedFileId;
    rec.file_path = itemDir.string();
    rec.storage_backend = storageBackend;
    rec.compat_tier = 1;
    rec.compat_score = 100;
    rec.aspect_ratio = 1.7777778f;
    rec.dominant_color = 0x2b303c;

    // Load project metadata using we_project_load_dir
    we_project_t proj;
    std::memset(&proj, 0, sizeof(proj));
    bool hasProject = we_project_load_dir(itemDir.string().c_str(), &proj);

    if (hasProject) {
        rec.title = proj.title[0] ? proj.title : itemDir.filename().string();
        rec.type = proj.type[0] ? proj.type : "scene";
        if (proj.preview[0]) {
            rec.preview_path = (itemDir / proj.preview).string();
        }
    } else {
        rec.title = itemDir.filename().string();
        rec.type = "scene";
    }

    // Locate preview file if not specified in project.json
    if (rec.preview_path.empty()) {
        const char *previewCandidates[] = {"preview.jpg", "preview.png", "preview.webp",
                                           "preview.gif", "preview_card.jpg"};
        for (const char *cand : previewCandidates) {
            auto candPath = itemDir / cand;
            if (std::filesystem::exists(candPath, ec)) {
                rec.preview_path = candPath.string();
                break;
            }
        }
    }

    // Deterministic 4-tier capability audit via we_compat_scanner (§5.8)
    we_compat_scanner_t *scanner = we_compat_scanner_create(true, true);
    we_compat_report_t *report = we_compat_report_alloc();
    if (scanner && report) {
        if (we_compat_scan_directory(scanner, itemDir.string().c_str(), report)) {
            rec.compat_tier = static_cast<uint32_t>(report->tier);
            rec.compat_score = report->compatibility_score;
            if (rec.title.empty() && report->title[0]) {
                rec.title = report->title;
            }
        }
    }
    if (report)
        we_compat_report_free(report);
    if (scanner)
        we_compat_scanner_destroy(scanner);

    // Compute aggregate size and latest mtime
    uint64_t totalBytes = 0;
    uint64_t latestMtime = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             itemDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (entry.is_regular_file(ec)) {
            totalBytes += entry.file_size(ec);
            auto ftime = entry.last_write_time(ec);
            auto s =
                std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
            if (static_cast<uint64_t>(s) > latestMtime)
                latestMtime = static_cast<uint64_t>(s);
        }
    }
    rec.file_size_bytes = totalBytes;
    rec.time_updated = latestMtime;
    rec.install_state = WAYWAL_UGC_STATE_INSTALLED;

    // Parse bitmask tags from title
    rec.tag_mask = WallpaperTags::parseTagsFromString(rec.title + " " + rec.tags_string);

    return upsertItem(rec);
}

} // namespace wwal::studio
