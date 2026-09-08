#include "datasearch/core/SourceRegistry.h"

#include <sqlite3.h>

#include <stdexcept>

namespace datasearch::core {

namespace {

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

} // namespace

SourceRegistry::SourceRegistry(const std::filesystem::path& registryDbPath) {
    if (sqlite3_open(registryDbPath.string().c_str(), &db_) != SQLITE_OK) {
        std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        if (db_ != nullptr) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("Failed to open source registry: " + message);
    }
    execOrThrow(db_,
                "CREATE TABLE IF NOT EXISTS sources ("
                "  root TEXT PRIMARY KEY,"
                "  db_path TEXT NOT NULL,"
                "  last_full_scan INTEGER NOT NULL DEFAULT 0"
                ");");
}

SourceRegistry::~SourceRegistry() {
    if (db_ != nullptr) sqlite3_close(db_);
}

void SourceRegistry::upsert(const SourceEntry& entry) {
    Statement stmt(db_,
                    "INSERT INTO sources(root, db_path, last_full_scan) VALUES(?1, ?2, ?3) "
                    "ON CONFLICT(root) DO UPDATE SET db_path=excluded.db_path;");
    sqlite3_bind_text(stmt, 1, entry.root.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, entry.dbPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(entry.lastFullScan));
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw std::runtime_error(std::string("Failed to upsert source: ") + sqlite3_errmsg(db_));
    }
}

void SourceRegistry::remove(const std::string& root) {
    Statement stmt(db_, "DELETE FROM sources WHERE root = ?1;");
    sqlite3_bind_text(stmt, 1, root.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
}

void SourceRegistry::touchLastFullScan(const std::string& root, std::int64_t when) {
    Statement stmt(db_, "UPDATE sources SET last_full_scan = ?1 WHERE root = ?2;");
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(when));
    sqlite3_bind_text(stmt, 2, root.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
}

std::vector<SourceEntry> SourceRegistry::all() const {
    std::vector<SourceEntry> result;
    Statement stmt(db_, "SELECT root, db_path, last_full_scan FROM sources ORDER BY root;");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SourceEntry entry;
        entry.root = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        entry.dbPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        entry.lastFullScan = sqlite3_column_int64(stmt, 2);
        result.push_back(std::move(entry));
    }
    return result;
}

} // namespace datasearch::core
