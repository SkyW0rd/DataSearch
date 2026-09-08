#include "datasearch/core/IndexStorage.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace datasearch::core {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Escapes '%', '_' and the escape character itself for a SQL LIKE '...' ESCAPE '\' clause.
std::string escapeLikePattern(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c == '%' || c == '_' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("sqlite3_prepare_v2 failed: ") + sqlite3_errmsg(db));
        }
    }
    ~Statement() { sqlite3_finalize(stmt_); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    operator sqlite3_stmt*() const { return stmt_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

void execOrThrow(sqlite3* db, const char* sql) {
    char* errMsg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::string message = errMsg != nullptr ? errMsg : "unknown SQLite error";
        sqlite3_free(errMsg);
        throw std::runtime_error("SQLite error: " + message);
    }
}

const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS files (
    path TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    name_lower TEXT NOT NULL,
    ext TEXT NOT NULL,
    size INTEGER NOT NULL,
    created_time INTEGER NOT NULL,
    modified_time INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_files_name_lower ON files(name_lower);
)SQL";

const char* sortColumn(SortField field) {
    switch (field) {
        case SortField::ModifiedTime: return "modified_time";
        case SortField::Size: return "size";
        case SortField::Name: default: return "name_lower";
    }
}

} // namespace

IndexStorage::IndexStorage(const std::filesystem::path& dbPath) {
    if (sqlite3_open(dbPath.string().c_str(), &db_) != SQLITE_OK) {
        std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        if (db_ != nullptr) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("Failed to open index database: " + message);
    }

    // WAL + NORMAL sync: readers never block on a writer, and a crash mid-write
    // can't corrupt the database (ТЗ NFR-5) — the last committed transaction stands.
    execOrThrow(db_, "PRAGMA journal_mode=WAL;");
    execOrThrow(db_, "PRAGMA synchronous=NORMAL;");
    execOrThrow(db_, "PRAGMA foreign_keys=ON;");
    execOrThrow(db_, kSchemaSql);
}

IndexStorage::~IndexStorage() {
    if (inBatch_) {
        commitBatch();
    }
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

void IndexStorage::beginBatch() {
    if (inBatch_) return;
    execOrThrow(db_, "BEGIN IMMEDIATE;");
    inBatch_ = true;
}

void IndexStorage::commitBatch() {
    if (!inBatch_) return;
    execOrThrow(db_, "COMMIT;");
    inBatch_ = false;
}

void IndexStorage::upsertFile(const FileRecord& record) {
    const bool ownTransaction = !inBatch_;
    if (ownTransaction) beginBatch();

    static const char* kSql =
        "INSERT INTO files(path, name, name_lower, ext, size, created_time, modified_time) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7) "
        "ON CONFLICT(path) DO UPDATE SET "
        "  name=excluded.name, name_lower=excluded.name_lower, ext=excluded.ext, "
        "  size=excluded.size, created_time=excluded.created_time, modified_time=excluded.modified_time;";

    Statement stmt(db_, kSql);
    const std::string nameLower = toLower(record.name);
    sqlite3_bind_text(stmt, 1, record.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, record.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, nameLower.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, record.extension.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(record.size));
    sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(record.createdTime));
    sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(record.modifiedTime));

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw std::runtime_error(std::string("Failed to upsert file: ") + sqlite3_errmsg(db_));
    }

    if (ownTransaction) commitBatch();
}

void IndexStorage::removeFile(const std::string& path) {
    const bool ownTransaction = !inBatch_;
    if (ownTransaction) beginBatch();

    Statement stmt(db_, "DELETE FROM files WHERE path = ?1;");
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw std::runtime_error(std::string("Failed to remove file: ") + sqlite3_errmsg(db_));
    }

    if (ownTransaction) commitBatch();
}

std::vector<FileRecord> IndexStorage::allRecords() const {
    std::vector<FileRecord> results;
    Statement stmt(db_, "SELECT path, name, ext, size, created_time, modified_time FROM files;");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FileRecord record;
        record.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        record.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.extension = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        record.size = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 3));
        record.createdTime = sqlite3_column_int64(stmt, 4);
        record.modifiedTime = sqlite3_column_int64(stmt, 5);
        results.push_back(std::move(record));
    }
    return results;
}

std::vector<FileRecord> IndexStorage::search(const SearchQuery& query) const {
    std::string sql =
        "SELECT path, name, ext, size, created_time, modified_time FROM files "
        "WHERE name_lower LIKE ?1 ESCAPE '\\' "
        "ORDER BY ";
    sql += sortColumn(query.sortField);
    sql += query.sortOrder == SortOrder::Descending ? " DESC" : " ASC";
    sql += " LIMIT ?2 OFFSET ?3;";

    Statement stmt(db_, sql.c_str());
    const std::string pattern = "%" + escapeLikePattern(toLower(query.namePattern)) + "%";
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, query.limit);
    sqlite3_bind_int(stmt, 3, query.offset);

    std::vector<FileRecord> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FileRecord record;
        record.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        record.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.extension = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        record.size = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 3));
        record.createdTime = sqlite3_column_int64(stmt, 4);
        record.modifiedTime = sqlite3_column_int64(stmt, 5);
        results.push_back(std::move(record));
    }
    return results;
}

std::uint64_t IndexStorage::fileCount() const {
    Statement stmt(db_, "SELECT COUNT(*) FROM files;");
    std::uint64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    }
    return count;
}

} // namespace datasearch::core
