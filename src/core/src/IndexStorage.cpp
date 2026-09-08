#include "datasearch/core/IndexStorage.h"

#include "datasearch/core/Fts5RussianTokenizer.h"
#include "datasearch/core/SearchQueryParser.h"

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

// files_fts is a self-contained FTS5 table (not content='files'): an earlier
// external-content design hit a reproducible "database disk image is
// malformed" error from bm25() specifically on external-content tables with
// this SQLite build (confirmed in isolation, unrelated to the custom
// tokenizer or to this project's own code) — self-contained duplicates
// name/content bytes into the FTS5 shadow storage but is the well-supported,
// documented configuration, and NFR-4's index-size budget already expects
// some FTS overhead.
const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS files (
    id INTEGER PRIMARY KEY,
    path TEXT UNIQUE NOT NULL,
    name TEXT NOT NULL,
    ext TEXT NOT NULL,
    size INTEGER NOT NULL,
    created_time INTEGER NOT NULL,
    modified_time INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_files_name ON files(name COLLATE NOCASE);

CREATE VIRTUAL TABLE IF NOT EXISTS files_fts USING fts5(
    name, content,
    tokenize='ru_snowball',
    prefix='2 3 4'
);
)SQL";

const char* sortColumnPlain(SortField field) {
    switch (field) {
        case SortField::ModifiedTime: return "modified_time";
        case SortField::Size: return "size";
        default: return "name COLLATE NOCASE";
    }
}

const char* sortColumnFts(SortField field) {
    switch (field) {
        case SortField::ModifiedTime: return "f.modified_time";
        case SortField::Size: return "f.size";
        case SortField::Name: return "f.name COLLATE NOCASE";
        default: return "bm25(files_fts, 10.0, 1.0)";
    }
}

std::string escapeFtsQuoted(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size() + 2);
    for (char c : text) {
        if (c == '"') escaped += "\"\"";
        else escaped += c;
    }
    return escaped;
}

// Builds an FTS5 MATCH expression ANDing every token: barewords become
// prefix-matched quoted phrases (практик -> "практик"*, ТЗ FR-10), tokens
// from "double quotes" in the original query become exact quoted phrases
// with no prefix wildcard (ТЗ FR-13). Assumes `tokens` is non-empty.
std::string buildMatchExpression(const std::vector<ParsedSearchQuery::Token>& tokens) {
    std::string expr;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) expr += " AND ";
        expr += "\"";
        expr += escapeFtsQuoted(tokens[i].text);
        expr += "\"";
        if (!tokens[i].isPhrase) expr += "*";
    }
    return expr;
}

std::string escapeLikePattern(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c == '%' || c == '_' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

FileRecord readRow(sqlite3_stmt* stmt, bool hasSnippet) {
    FileRecord record;
    record.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    record.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    record.extension = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    record.size = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 3));
    record.createdTime = sqlite3_column_int64(stmt, 4);
    record.modifiedTime = sqlite3_column_int64(stmt, 5);
    if (hasSnippet) {
        const unsigned char* snip = sqlite3_column_text(stmt, 6);
        record.snippet = snip != nullptr ? reinterpret_cast<const char*>(snip) : "";
        record.relevanceScore = sqlite3_column_double(stmt, 7);
    }
    return record;
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

    if (!registerRussianFts5Tokenizer(db_)) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("Failed to register the 'ru_snowball' FTS5 tokenizer "
                                  "(this SQLite build may lack FTS5)");
    }

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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (inBatch_) return;
    execOrThrow(db_, "BEGIN IMMEDIATE;");
    inBatch_ = true;
}

void IndexStorage::commitBatch() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!inBatch_) return;
    execOrThrow(db_, "COMMIT;");
    inBatch_ = false;
}

void IndexStorage::upsertFile(const FileRecord& record, const std::string& content) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const bool ownTransaction = !inBatch_;
    if (ownTransaction) beginBatch();

    static const char* kUpsertSql =
        "INSERT INTO files(path, name, ext, size, created_time, modified_time) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(path) DO UPDATE SET "
        "  name=excluded.name, ext=excluded.ext, size=excluded.size, "
        "  created_time=excluded.created_time, modified_time=excluded.modified_time "
        "RETURNING id;";

    sqlite3_int64 rowId = 0;
    {
        Statement stmt(db_, kUpsertSql);
        sqlite3_bind_text(stmt, 1, record.path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, record.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, record.extension.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(record.size));
        sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(record.createdTime));
        sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(record.modifiedTime));

        if (sqlite3_step(stmt) != SQLITE_ROW) {
            throw std::runtime_error(std::string("Failed to upsert file: ") + sqlite3_errmsg(db_));
        }
        rowId = sqlite3_column_int64(stmt, 0);
    }

    {
        // Self-contained FTS5 table, kept in sync manually: drop any previous
        // row for this id, then re-insert with the current name/content.
        Statement del(db_, "DELETE FROM files_fts WHERE rowid = ?1;");
        sqlite3_bind_int64(del, 1, rowId);
        sqlite3_step(del);
    }
    {
        Statement ins(db_, "INSERT INTO files_fts(rowid, name, content) VALUES (?1, ?2, ?3);");
        sqlite3_bind_int64(ins, 1, rowId);
        sqlite3_bind_text(ins, 2, record.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, content.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(ins) != SQLITE_DONE) {
            throw std::runtime_error(std::string("Failed to update FTS index: ") + sqlite3_errmsg(db_));
        }
    }

    if (ownTransaction) commitBatch();
}

void IndexStorage::removeFile(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const bool ownTransaction = !inBatch_;
    if (ownTransaction) beginBatch();

    sqlite3_int64 rowId = -1;
    {
        Statement sel(db_, "SELECT id FROM files WHERE path = ?1;");
        sqlite3_bind_text(sel, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(sel) == SQLITE_ROW) rowId = sqlite3_column_int64(sel, 0);
    }
    if (rowId >= 0) {
        Statement delFts(db_, "DELETE FROM files_fts WHERE rowid = ?1;");
        sqlite3_bind_int64(delFts, 1, rowId);
        sqlite3_step(delFts);
    }
    {
        Statement del(db_, "DELETE FROM files WHERE path = ?1;");
        sqlite3_bind_text(del, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(del) != SQLITE_DONE) {
            throw std::runtime_error(std::string("Failed to remove file: ") + sqlite3_errmsg(db_));
        }
    }

    if (ownTransaction) commitBatch();
}

std::vector<FileRecord> IndexStorage::allRecords() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<FileRecord> results;
    Statement stmt(db_, "SELECT path, name, ext, size, created_time, modified_time FROM files;");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(readRow(stmt, /*hasSnippet=*/false));
    }
    return results;
}

std::vector<FileRecord> IndexStorage::search(const SearchQuery& query) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<FileRecord> results;

    // ТЗ FR-13: точная фраза в кавычках, исключение через "-", ext:/path: —
    // parsed once here so both the FTS5 and the plain "browse all" paths
    // below can honor the same filters consistently.
    const ParsedSearchQuery parsed = parseSearchQuery(query.namePattern);
    std::vector<ParsedSearchQuery::Token> positive;
    std::vector<ParsedSearchQuery::Token> negative;
    for (const auto& token : parsed.tokens) {
        (token.excluded ? negative : positive).push_back(token);
    }

    const std::string positiveExpr = positive.empty() ? std::string() : buildMatchExpression(positive);
    const std::string negativeExpr = negative.empty() ? std::string() : buildMatchExpression(negative);
    const std::string likePattern =
        parsed.pathFilter ? "%" + escapeLikePattern(*parsed.pathFilter) + "%" : std::string();

    std::vector<std::string> extraWhere;
    if (!negativeExpr.empty()) extraWhere.push_back("f.id NOT IN (SELECT rowid FROM files_fts WHERE files_fts MATCH ?)");
    if (parsed.extensionFilter) extraWhere.push_back("LOWER(f.ext) = ?");
    if (parsed.pathFilter) extraWhere.push_back("f.path LIKE ? ESCAPE '\\'");

    auto bindExtras = [&](Statement& stmt, int& idx) {
        if (!negativeExpr.empty()) sqlite3_bind_text(stmt, idx++, negativeExpr.c_str(), -1, SQLITE_TRANSIENT);
        if (parsed.extensionFilter) sqlite3_bind_text(stmt, idx++, parsed.extensionFilter->c_str(), -1, SQLITE_TRANSIENT);
        if (parsed.pathFilter) sqlite3_bind_text(stmt, idx++, likePattern.c_str(), -1, SQLITE_TRANSIENT);
    };

    if (!positiveExpr.empty()) {
        std::string sql =
            "SELECT f.path, f.name, f.ext, f.size, f.created_time, f.modified_time, "
            "       snippet(files_fts, 1, '[', ']', '...', 12), "
            "       bm25(files_fts, 10.0, 1.0) "
            "FROM files_fts JOIN files f ON f.id = files_fts.rowid "
            "WHERE files_fts MATCH ?";
        for (const auto& clause : extraWhere) {
            sql += " AND ";
            sql += clause;
        }
        sql += " ORDER BY ";
        sql += sortColumnFts(query.sortField);
        sql += query.sortOrder == SortOrder::Descending ? " DESC" : " ASC";
        sql += " LIMIT ? OFFSET ?;";

        Statement stmt(db_, sql.c_str());
        int idx = 1;
        sqlite3_bind_text(stmt, idx++, positiveExpr.c_str(), -1, SQLITE_TRANSIENT);
        bindExtras(stmt, idx);
        sqlite3_bind_int(stmt, idx++, query.limit);
        sqlite3_bind_int(stmt, idx++, query.offset);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(readRow(stmt, /*hasSnippet=*/true));
        }
        return results;
    }

    // No positive full-text terms (browsing all, or only -exclusions/ext:/path:
    // filters): plain scan over `files`, still honoring whichever filters
    // were given.
    std::string sql = "SELECT path, name, ext, size, created_time, modified_time FROM files f";
    if (!extraWhere.empty()) {
        sql += " WHERE ";
        for (std::size_t i = 0; i < extraWhere.size(); ++i) {
            if (i > 0) sql += " AND ";
            sql += extraWhere[i];
        }
    }
    sql += " ORDER BY ";
    sql += sortColumnPlain(query.sortField);
    sql += query.sortOrder == SortOrder::Descending ? " DESC" : " ASC";
    sql += " LIMIT ? OFFSET ?;";

    Statement stmt(db_, sql.c_str());
    int idx = 1;
    bindExtras(stmt, idx);
    sqlite3_bind_int(stmt, idx++, query.limit);
    sqlite3_bind_int(stmt, idx++, query.offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(readRow(stmt, /*hasSnippet=*/false));
    }
    return results;
}

std::uint64_t IndexStorage::fileCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Statement stmt(db_, "SELECT COUNT(*) FROM files;");
    std::uint64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    }
    return count;
}

} // namespace datasearch::core
