#include "datasearch/core/IndexStorage.h"

#include "datasearch/core/Fts5RussianTokenizer.h"
#include "datasearch/core/SearchQueryParser.h"
#include "datasearch/core/Utf8.h"

#include <sqlite3.h>

#include <algorithm>
#include <optional>
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

CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL);
)SQL";

// Exact-word index: contentless (the text itself is already stored once, in
// files_fts), so rows are removed with FTS5's 'delete' command, which needs
// the exact values that were indexed — taken from files_fts.
const char* kExactSchemaSql =
    "CREATE VIRTUAL TABLE files_exact USING fts5(name, content, content='', tokenize='ru_exact');";

bool tableExists(sqlite3* db, const char* name) {
    Statement stmt(db, "SELECT 1 FROM sqlite_master WHERE name = ?1;");
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    return sqlite3_step(stmt) == SQLITE_ROW;
}

std::int64_t readMeta(sqlite3* db, const char* key) {
    Statement stmt(db, "SELECT value FROM meta WHERE key = ?1;");
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    return sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
}

void writeMeta(sqlite3* db, const char* key, std::int64_t value) {
    Statement stmt(db, "INSERT OR REPLACE INTO meta(key, value) VALUES(?1, ?2);");
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, value);
    sqlite3_step(stmt);
}

void deleteMeta(sqlite3* db, const char* key) {
    Statement stmt(db, "DELETE FROM meta WHERE key = ?1;");
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
}

// --- Exact-mode excerpts ----------------------------------------------------
// Built from the stored text rather than by FTS5's snippet(), which only
// understands the stemmed index and would highlight other word forms.

// Next code point at `i` (advancing `i`); invalid bytes decode as themselves.
std::uint32_t nextCodePoint(const std::string& s, std::size_t& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    int extra = b0 >= 0xF0 ? 3 : b0 >= 0xE0 ? 2 : b0 >= 0xC0 ? 1 : 0;
    if (i + extra >= s.size()) extra = 0;
    std::uint32_t cp = extra == 0 ? b0 : b0 & (0x3F >> extra);
    for (int k = 1; k <= extra; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += 1 + extra;
    return cp;
}

// Lowercase, with ё folded to е — matching what the "ru_exact" tokenizer does.
std::uint32_t foldCodePoint(std::uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 0x20;
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;           // А-Я
    if (cp == 0x0401 || cp == 0x0451) return 0x0435;               // Ё, ё -> е
    if (cp >= 0x0400 && cp <= 0x040F) return cp + 0x50;           // Ѐ-Џ
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;  // Latin-1 capitals
    return cp;
}

bool isWordCodePoint(std::uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
           (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7) || (cp >= 0x0400 && cp <= 0x04FF);
}

std::u32string foldedWord(const std::string& utf8) {
    std::u32string out;
    for (std::size_t i = 0; i < utf8.size();) out.push_back(foldCodePoint(nextCodePoint(utf8, i)));
    return out;
}

// Finds the next word at or after `pos` (below `limit`): its byte range and
// folded form. False when there are no more words.
bool nextWord(const std::string& text, std::size_t limit, std::size_t& pos, std::size_t& begin, std::size_t& end,
              std::u32string& folded) {
    while (pos < limit) {
        begin = pos;
        const std::uint32_t cp = nextCodePoint(text, pos);
        if (!isWordCodePoint(cp)) continue;
        folded.assign(1, foldCodePoint(cp));
        end = pos;
        while (end < limit) {
            std::size_t next = end;
            const std::uint32_t c = nextCodePoint(text, next);
            if (!isWordCodePoint(c)) break;
            folded.push_back(foldCodePoint(c));
            end = next;
        }
        pos = end;
        return true;
    }
    return false;
}

// About a dozen words around the first whole-word match of any of `words`,
// matches wrapped in [ ] like FTS5's snippet(). If the text has no match (the
// hit was in the file name), its opening words instead.
std::string exactSnippet(const std::string& text, const std::vector<std::u32string>& words) {
    constexpr std::size_t kBefore = 5;
    constexpr std::size_t kAfter = 12;
    // A file can hold hundreds of MB of text; the excerpt comes from the
    // first stretch with a hit, so there's no need to read on and on.
    const std::size_t limit = std::min<std::size_t>(text.size(), 16u * 1024 * 1024);

    struct Word { std::size_t begin, end; bool hit; };
    std::vector<Word> window;  // up to kBefore words, then the hit and what follows it
    std::size_t hitIndex = std::string::npos;
    std::size_t pos = 0, begin = 0, end = 0;
    std::u32string folded;
    while (nextWord(text, limit, pos, begin, end, folded)) {
        const bool hit = std::find(words.begin(), words.end(), folded) != words.end();
        window.push_back({begin, end, hit});
        if (hitIndex == std::string::npos) {
            if (hit) hitIndex = window.size() - 1;
            else if (window.size() > kBefore) window.erase(window.begin());
        } else if (window.size() >= hitIndex + 1 + kAfter) {
            break;
        }
    }

    if (hitIndex == std::string::npos) {
        std::size_t p = 0, wordEnd = 0, count = 0;
        while (count < kBefore + kAfter && nextWord(text, limit, p, begin, end, folded)) {
            wordEnd = end;
            ++count;
        }
        if (count == 0) return {};
        return text.substr(0, wordEnd) + (wordEnd < text.size() ? "..." : "");
    }

    std::string out = window.front().begin > 0 ? "..." : "";
    std::size_t copied = window.front().begin;
    for (const auto& w : window) {
        out.append(text, copied, w.begin - copied);
        if (w.hit) out += '[';
        out.append(text, w.begin, w.end - w.begin);
        if (w.hit) out += ']';
        copied = w.end;
    }
    if (copied < text.size()) out += "...";
    return out;
}

const char* sortColumnPlain(SortField field) {
    switch (field) {
        case SortField::ModifiedTime: return "modified_time";
        case SortField::Size: return "size";
        default: return "name COLLATE NOCASE";
    }
}

std::string sortColumnFts(SortField field, const std::string& table) {
    switch (field) {
        case SortField::ModifiedTime: return "f.modified_time";
        case SortField::Size: return "f.size";
        case SortField::Name: return "f.name COLLATE NOCASE";
        default: return "bm25(" + table + ", 10.0, 1.0)";
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
// `exact`: no prefix wildcard either — whole words only, for files_exact.
std::string buildMatchExpression(const std::vector<ParsedSearchQuery::Token>& tokens, bool exact) {
    std::string expr;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) expr += " AND ";
        expr += "\"";
        expr += escapeFtsQuoted(tokens[i].text);
        expr += "\"";
        if (!tokens[i].isPhrase && !exact) expr += "*";
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
    // SQLite takes UTF-8 file names; path::string() is the ANSI code page on
    // Windows, which breaks for a Cyrillic user name in the AppData path.
    const std::string pathText = pathToUtf8(dbPath);
    if (sqlite3_open(pathText.c_str(), &db_) != SQLITE_OK) {
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

    if (!tableExists(db_, "files_exact")) {
        execOrThrow(db_, kExactSchemaSql);
        // An index from before files_exact existed: its files are filled in
        // later from the text already stored in files_fts (see Indexer).
        Statement maxRow(db_, "SELECT IFNULL(MAX(rowid), 0) FROM files_fts;");
        const std::int64_t until = sqlite3_step(maxRow) == SQLITE_ROW ? sqlite3_column_int64(maxRow, 0) : 0;
        if (until > 0) {
            writeMeta(db_, "exact_backfill_until", until);
            writeMeta(db_, "exact_backfill_done", 0);
        }
    }
    backfillUntil_ = readMeta(db_, "exact_backfill_until");
    backfillDone_ = readMeta(db_, "exact_backfill_done");

    readDb_ = db_;
    if (!pathText.empty() && pathText != ":memory:") {
        sqlite3* reader = nullptr;
        if (sqlite3_open(pathText.c_str(), &reader) == SQLITE_OK && registerRussianFts5Tokenizer(reader)) {
            execOrThrow(reader, "PRAGMA query_only=1;");
            readDb_ = reader;
        } else if (reader != nullptr) {
            sqlite3_close(reader);  // searches share the writer connection then
        }
    }
}

IndexStorage::~IndexStorage() {
    if (inBatch_) {
        commitBatch();
    }
    if (readDb_ != nullptr && readDb_ != db_) {
        sqlite3_close(readDb_);
    }
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

bool IndexStorage::inExactIndex(std::int64_t rowId) const {
    return !(rowId > backfillDone_ && rowId <= backfillUntil_);
}

std::unique_lock<std::recursive_mutex> IndexStorage::lockRead() const {
    return std::unique_lock<std::recursive_mutex>(readDb_ == db_ ? mutex_ : readMutex_);
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

    // Deliberately not one "INSERT ... ON CONFLICT DO UPDATE ... RETURNING":
    // SQLite runs that statement inside a statement savepoint, and FTS5
    // flushes its in-memory pending-terms table on every savepoint. The cost
    // of a flush scales with that table's slot count, which grows to millions
    // after one huge document and never shrinks — so every later file paid
    // for it, and indexing slowed ~20x from the first big file onward.
    sqlite3_int64 rowId = -1;
    {
        Statement sel(db_, "SELECT id FROM files WHERE path = ?1;");
        sqlite3_bind_text(sel, 1, record.path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(sel) == SQLITE_ROW) rowId = sqlite3_column_int64(sel, 0);
    }

    auto bindMetadata = [&](sqlite3_stmt* stmt) {
        sqlite3_bind_text(stmt, 1, record.path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, record.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, record.extension.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(record.size));
        sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(record.createdTime));
        sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(record.modifiedTime));
    };

    if (rowId >= 0) {
        {
            Statement upd(db_,
                          "UPDATE files SET path=?1, name=?2, ext=?3, size=?4, created_time=?5, modified_time=?6 "
                          "WHERE id=?7;");
            bindMetadata(upd);
            sqlite3_bind_int64(upd, 7, rowId);
            if (sqlite3_step(upd) != SQLITE_DONE) {
                throw std::runtime_error(std::string("Failed to update file: ") + sqlite3_errmsg(db_));
            }
        }
        // Self-contained FTS5 table, kept in sync manually: drop the previous
        // row for this id before re-inserting the current name/content.
        removeFromExactIndex(rowId);
        Statement del(db_, "DELETE FROM files_fts WHERE rowid = ?1;");
        sqlite3_bind_int64(del, 1, rowId);
        sqlite3_step(del);
    } else {
        Statement ins(db_,
                      "INSERT INTO files(path, name, ext, size, created_time, modified_time) "
                      "VALUES(?1, ?2, ?3, ?4, ?5, ?6);");
        bindMetadata(ins);
        if (sqlite3_step(ins) != SQLITE_DONE) {
            throw std::runtime_error(std::string("Failed to insert file: ") + sqlite3_errmsg(db_));
        }
        rowId = sqlite3_last_insert_rowid(db_);
    }

    {
        Statement ins(db_, "INSERT INTO files_fts(rowid, name, content) VALUES (?1, ?2, ?3);");
        sqlite3_bind_int64(ins, 1, rowId);
        sqlite3_bind_text(ins, 2, record.name.c_str(), -1, SQLITE_TRANSIENT);
        // STATIC: `content` outlives this statement, so no need for SQLite to
        // copy it — for a large document that copy doubled peak memory.
        sqlite3_bind_text(ins, 3, content.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(ins) != SQLITE_DONE) {
            throw std::runtime_error(std::string("Failed to update FTS index: ") + sqlite3_errmsg(db_));
        }
    }
    if (inExactIndex(rowId)) {
        Statement ins(db_, "INSERT INTO files_exact(rowid, name, content) VALUES (?1, ?2, ?3);");
        sqlite3_bind_int64(ins, 1, rowId);
        sqlite3_bind_text(ins, 2, record.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, content.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(ins) != SQLITE_DONE) {
            throw std::runtime_error(std::string("Failed to update exact index: ") + sqlite3_errmsg(db_));
        }
    }

    if (ownTransaction) commitBatch();
}

// files_exact is contentless: a row comes out through FTS5's 'delete'
// command given exactly the values it was indexed with, i.e. what files_fts
// still holds for it — so this must run before the files_fts row goes.
void IndexStorage::removeFromExactIndex(std::int64_t rowId) {
    if (!inExactIndex(rowId)) return;
    Statement old(db_, "SELECT name, content FROM files_fts WHERE rowid = ?1;");
    sqlite3_bind_int64(old, 1, rowId);
    if (sqlite3_step(old) != SQLITE_ROW) return;
    Statement del(db_, "INSERT INTO files_exact(files_exact, rowid, name, content) VALUES('delete', ?1, ?2, ?3);");
    sqlite3_bind_int64(del, 1, rowId);
    sqlite3_bind_value(del, 2, sqlite3_column_value(old, 0));
    sqlite3_bind_value(del, 3, sqlite3_column_value(old, 1));
    if (sqlite3_step(del) != SQLITE_DONE) {
        throw std::runtime_error(std::string("Failed to update exact index: ") + sqlite3_errmsg(db_));
    }
}

std::uint64_t IndexStorage::exactBackfillRemaining() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (backfillDone_ >= backfillUntil_) return 0;
    Statement stmt(db_, "SELECT COUNT(*) FROM files_fts WHERE rowid > ?1 AND rowid <= ?2;");
    sqlite3_bind_int64(stmt, 1, backfillDone_);
    sqlite3_bind_int64(stmt, 2, backfillUntil_);
    return sqlite3_step(stmt) == SQLITE_ROW ? static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0)) : 0;
}

std::uint64_t IndexStorage::backfillExactIndex(std::uint64_t maxRows) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (backfillDone_ >= backfillUntil_) return 0;
    const bool ownTransaction = !inBatch_;
    if (ownTransaction) beginBatch();

    std::int64_t upTo = backfillUntil_;
    std::uint64_t rows = 0;
    {
        Statement range(db_,
                        "SELECT MAX(rowid), COUNT(*) FROM (SELECT rowid FROM files_fts "
                        "WHERE rowid > ?1 AND rowid <= ?2 ORDER BY rowid LIMIT ?3);");
        sqlite3_bind_int64(range, 1, backfillDone_);
        sqlite3_bind_int64(range, 2, backfillUntil_);
        sqlite3_bind_int64(range, 3, static_cast<sqlite3_int64>(maxRows));
        if (sqlite3_step(range) == SQLITE_ROW && sqlite3_column_int64(range, 1) > 0) {
            upTo = sqlite3_column_int64(range, 0);
            rows = static_cast<std::uint64_t>(sqlite3_column_int64(range, 1));
        }
    }
    if (rows > 0) {
        Statement fill(db_,
                       "INSERT INTO files_exact(rowid, name, content) "
                       "SELECT rowid, name, content FROM files_fts WHERE rowid > ?1 AND rowid <= ?2;");
        sqlite3_bind_int64(fill, 1, backfillDone_);
        sqlite3_bind_int64(fill, 2, upTo);
        if (sqlite3_step(fill) != SQLITE_DONE) {
            throw std::runtime_error(std::string("Failed to fill exact index: ") + sqlite3_errmsg(db_));
        }
    }

    if (upTo >= backfillUntil_) {
        deleteMeta(db_, "exact_backfill_until");
        deleteMeta(db_, "exact_backfill_done");
        backfillDone_ = backfillUntil_ = 0;
    } else {
        writeMeta(db_, "exact_backfill_done", upTo);
        backfillDone_ = upTo;
    }
    if (ownTransaction) commitBatch();
    return rows;
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
        removeFromExactIndex(rowId);
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

namespace {

// A search-box query turned into SQL pieces, shared by search(), the count
// and per-file excerpts so all three agree on what matches. ТЗ FR-13:
// "точная фраза" в кавычках, исключение через "-", ext:/path: фильтры.
struct QueryPlan {
    std::vector<ParsedSearchQuery::Token> positive;
    std::string table;  // files_exact for whole-word search, files_fts for word forms
    std::string positiveExpr;
    std::string negativeExpr;
    std::optional<std::string> extensionFilter;
    std::string likePattern;
    std::vector<std::string> extraWhere;
};

QueryPlan planQuery(const SearchQuery& query) {
    const ParsedSearchQuery parsed = parseSearchQuery(query.namePattern);
    QueryPlan plan;
    plan.table = query.exactWords ? "files_exact" : "files_fts";
    std::vector<ParsedSearchQuery::Token> negative;
    for (const auto& token : parsed.tokens) (token.excluded ? negative : plan.positive).push_back(token);
    if (!plan.positive.empty()) plan.positiveExpr = buildMatchExpression(plan.positive, query.exactWords);
    if (!negative.empty()) plan.negativeExpr = buildMatchExpression(negative, query.exactWords);
    plan.extensionFilter = parsed.extensionFilter;
    if (parsed.pathFilter) plan.likePattern = "%" + escapeLikePattern(*parsed.pathFilter) + "%";

    if (!plan.negativeExpr.empty()) {
        plan.extraWhere.push_back("f.id NOT IN (SELECT rowid FROM " + plan.table + " WHERE " + plan.table + " MATCH ?)");
    }
    if (plan.extensionFilter) plan.extraWhere.push_back("LOWER(f.ext) = ?");
    if (parsed.pathFilter) plan.extraWhere.push_back("f.path LIKE ? ESCAPE '\\'");
    return plan;
}

void bindExtras(const QueryPlan& plan, sqlite3_stmt* stmt, int& idx) {
    if (!plan.negativeExpr.empty()) sqlite3_bind_text(stmt, idx++, plan.negativeExpr.c_str(), -1, SQLITE_TRANSIENT);
    if (plan.extensionFilter) sqlite3_bind_text(stmt, idx++, plan.extensionFilter->c_str(), -1, SQLITE_TRANSIENT);
    if (!plan.likePattern.empty()) sqlite3_bind_text(stmt, idx++, plan.likePattern.c_str(), -1, SQLITE_TRANSIENT);
}

std::string joinWhere(const std::vector<std::string>& clauses, const char* leading) {
    std::string sql;
    for (std::size_t i = 0; i < clauses.size(); ++i) {
        sql += i == 0 ? leading : " AND ";
        sql += clauses[i];
    }
    return sql;
}

// The words of the positive terms, folded like the "ru_exact" tokenizer.
std::vector<std::u32string> exactWordsOf(const std::vector<ParsedSearchQuery::Token>& tokens) {
    std::vector<std::u32string> words;
    for (const auto& token : tokens) {
        std::size_t pos = 0, begin = 0, end = 0;
        std::u32string folded;
        while (nextWord(token.text, token.text.size(), pos, begin, end, folded)) words.push_back(folded);
    }
    return words;
}

std::string storedText(sqlite3* db, std::int64_t rowId) {
    Statement stmt(db, "SELECT content FROM files_fts WHERE rowid = ?1;");
    sqlite3_bind_int64(stmt, 1, rowId);
    if (sqlite3_step(stmt) != SQLITE_ROW) return {};
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    return text != nullptr ? std::string(text, static_cast<std::size_t>(sqlite3_column_bytes(stmt, 0))) : std::string();
}

} // namespace

std::vector<FileRecord> IndexStorage::search(const SearchQuery& query) const {
    auto lock = lockRead();
    std::vector<FileRecord> results;
    const QueryPlan plan = planQuery(query);

    if (!plan.positiveExpr.empty()) {
        // FTS5's snippet() understands only the stemmed index; exact-mode
        // excerpts are built from the stored text instead (below).
        const bool ftsSnippet = query.withSnippets && !query.exactWords;
        const std::string& t = plan.table;
        std::string sql = "SELECT f.path, f.name, f.ext, f.size, f.created_time, f.modified_time, ";
        sql += ftsSnippet ? "snippet(files_fts, 1, '[', ']', '...', 12), " : "'', ";
        sql += "bm25(" + t + ", 10.0, 1.0) FROM " + t + " JOIN files f ON f.id = " + t + ".rowid WHERE " + t +
               " MATCH ?";
        sql += joinWhere(plan.extraWhere, " AND ");
        sql += " ORDER BY " + sortColumnFts(query.sortField, t);
        sql += query.sortOrder == SortOrder::Descending ? " DESC" : " ASC";
        sql += " LIMIT ? OFFSET ?;";

        Statement stmt(readDb_, sql.c_str());
        int idx = 1;
        sqlite3_bind_text(stmt, idx++, plan.positiveExpr.c_str(), -1, SQLITE_TRANSIENT);
        bindExtras(plan, stmt, idx);
        sqlite3_bind_int(stmt, idx++, query.limit);
        sqlite3_bind_int(stmt, idx++, query.offset);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(readRow(stmt, /*hasSnippet=*/true));
        }

        if (query.withSnippets && query.exactWords) {
            const auto words = exactWordsOf(plan.positive);
            for (auto& record : results) {
                Statement id(readDb_, "SELECT id FROM files WHERE path = ?1;");
                sqlite3_bind_text(id, 1, record.path.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(id) == SQLITE_ROW) {
                    record.snippet = exactSnippet(storedText(readDb_, sqlite3_column_int64(id, 0)), words);
                }
            }
        }
        return results;
    }

    // No positive full-text terms (browsing all, or only -exclusions/ext:/path:
    // filters): plain scan over `files`, still honoring whichever filters
    // were given.
    std::string sql = "SELECT path, name, ext, size, created_time, modified_time FROM files f";
    sql += joinWhere(plan.extraWhere, " WHERE ");
    sql += " ORDER BY ";
    sql += sortColumnPlain(query.sortField);
    sql += query.sortOrder == SortOrder::Descending ? " DESC" : " ASC";
    sql += " LIMIT ? OFFSET ?;";

    Statement stmt(readDb_, sql.c_str());
    int idx = 1;
    bindExtras(plan, stmt, idx);
    sqlite3_bind_int(stmt, idx++, query.limit);
    sqlite3_bind_int(stmt, idx++, query.offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(readRow(stmt, /*hasSnippet=*/false));
    }
    return results;
}

std::uint64_t IndexStorage::countMatches(const SearchQuery& query) const {
    auto lock = lockRead();
    const QueryPlan plan = planQuery(query);
    std::string sql;
    if (!plan.positiveExpr.empty()) {
        const std::string& t = plan.table;
        sql = "SELECT COUNT(*) FROM " + t + " JOIN files f ON f.id = " + t + ".rowid WHERE " + t + " MATCH ?";
        sql += joinWhere(plan.extraWhere, " AND ");
    } else {
        sql = "SELECT COUNT(*) FROM files f" + joinWhere(plan.extraWhere, " WHERE ");
    }
    Statement stmt(readDb_, sql.c_str());
    int idx = 1;
    if (!plan.positiveExpr.empty()) sqlite3_bind_text(stmt, idx++, plan.positiveExpr.c_str(), -1, SQLITE_TRANSIENT);
    bindExtras(plan, stmt, idx);
    return sqlite3_step(stmt) == SQLITE_ROW ? static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0)) : 0;
}

std::string IndexStorage::snippet(const std::string& path, const SearchQuery& query) const {
    auto lock = lockRead();
    const QueryPlan plan = planQuery(query);
    if (plan.positive.empty()) return {};

    std::int64_t rowId = -1;
    {
        Statement id(readDb_, "SELECT id FROM files WHERE path = ?1;");
        sqlite3_bind_text(id, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(id) == SQLITE_ROW) rowId = sqlite3_column_int64(id, 0);
    }
    if (rowId < 0) return {};

    if (query.exactWords) return exactSnippet(storedText(readDb_, rowId), exactWordsOf(plan.positive));

    Statement stmt(readDb_,
                   "SELECT snippet(files_fts, 1, '[', ']', '...', 12) FROM files_fts "
                   "WHERE files_fts MATCH ?1 AND rowid = ?2;");
    sqlite3_bind_text(stmt, 1, plan.positiveExpr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, rowId);
    if (sqlite3_step(stmt) != SQLITE_ROW) return {};
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    return text != nullptr ? text : "";
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
