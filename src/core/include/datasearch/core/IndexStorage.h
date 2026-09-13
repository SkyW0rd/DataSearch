#pragma once

#include "datasearch/core/FileRecord.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace datasearch::core {

enum class SortField { Relevance, Name, ModifiedTime, Size };
enum class SortOrder { Ascending, Descending };

struct SearchQuery {
    // Raw search-box text, parsed internally (see SearchQueryParser.h) for
    // ТЗ FR-13 operators: "exact phrase", -excluded, ext:docx, path:D:\Work\,
    // plus plain barewords (prefix-matched substring, ТЗ FR-10/FR-11). Empty
    // means "browse all files" (no ranking, just the plain file list).
    std::string namePattern;
    SortField sortField = SortField::Relevance;
    SortOrder sortOrder = SortOrder::Ascending;
    int limit = 200;
    int offset = 0;

    // true: whole words exactly as typed, case- and ё/е-insensitive
    // ("Михайлов" finds "михайлов", not "Михайлова"/"Михайленко"). false:
    // Russian word forms and prefixes too (ТЗ п.11.3: "практикум" finds
    // "практикума"; "практик" finds "практикум").
    bool exactWords = false;

    // Highlighted excerpts in search() results. Building one means re-reading
    // that file's whole text, which for a few hundred matches took seconds —
    // interactive callers pass false and fetch excerpts per visible row
    // through IndexStorage::snippet().
    bool withSnippets = true;
};

// One SQLite database = the index for a single source (one disk/folder root —
// ТЗ п.11.2: "индекс каждого диска хранится отдельно"). Backed by an FTS5
// table (name + extracted content, ТЗ п.5.1 вариант A) using the custom
// 'ru_snowball' tokenizer (see Fts5RussianTokenizer.h) for Russian morphology
// (ТЗ п.11.3), with name weighted above content in BM25 ranking (ТЗ п.8).
// Safe to call from multiple threads concurrently. Writes (and the
// reconcile snapshot) go through one connection under a mutex that also
// guards the batch-transaction bookkeeping; searches and excerpts use a
// second, read-only connection with its own lock, so a search never waits
// behind indexing (WAL lets it read the last committed state meanwhile).
// An in-memory database (":memory:") can't be shared between connections
// and uses the one connection for both.
//
// Next to the stemmed index (`files_fts`, which also keeps each file's text)
// there's a contentless exact-word index (`files_exact`, "ru_exact"
// tokenizer). An index created before `files_exact` existed gets it filled
// from the stored text — no files are re-read — via backfillExactIndex(),
// which Indexer drives in the background.
class IndexStorage {
public:
    explicit IndexStorage(const std::filesystem::path& dbPath);
    ~IndexStorage();

    IndexStorage(const IndexStorage&) = delete;
    IndexStorage& operator=(const IndexStorage&) = delete;

    // Wrap a run of upsertFile/removeFile calls in a transaction for throughput.
    // Safe to call without a batch too (each call becomes its own transaction).
    void beginBatch();
    void commitBatch();

    // `content` is the extracted full text (ТЗ п.3.1.1), empty for files with
    // nothing extractable — those stay searchable by name/metadata only.
    void upsertFile(const FileRecord& record, const std::string& content = {});
    void removeFile(const std::string& path);

    // Full dump of indexed paths + their stored metadata — used for the
    // startup metadata-reconciliation pass (ТЗ п.13.2), not for interactive search.
    std::vector<FileRecord> allRecords() const;

    // `record.snippet` is populated (highlighted excerpt, ТЗ FR-15) only when
    // namePattern is non-empty and query.withSnippets is set; browsing all
    // files leaves it empty.
    std::vector<FileRecord> search(const SearchQuery& query) const;

    // How many files match in total, ignoring limit/offset.
    std::uint64_t countMatches(const SearchQuery& query) const;

    // Highlighted excerpt for one file matched by `query` — for filling in
    // excerpts only for the rows actually on screen. Empty if the file isn't
    // indexed or has no text.
    std::string snippet(const std::string& path, const SearchQuery& query) const;

    std::uint64_t fileCount() const;

    // Filling `files_exact` for an index created before it existed: files
    // still to do, and one step of at most `maxRows` files (returns how many
    // were done; 0 once complete). Until it finishes, exact-word search only
    // sees the files already filled in (and anything indexed since).
    std::uint64_t exactBackfillRemaining() const;
    std::uint64_t backfillExactIndex(std::uint64_t maxRows);

private:
    // Rows in (backfillDone_, backfillUntil_] predate files_exact and aren't
    // in it yet: writes to them must not touch files_exact — deleting what
    // was never added would corrupt it — and the backfill picks up their
    // current text when it gets there.
    bool inExactIndex(std::int64_t rowId) const;
    void removeFromExactIndex(std::int64_t rowId);
    std::unique_lock<std::recursive_mutex> lockRead() const;

    mutable std::recursive_mutex mutex_;
    sqlite3* db_ = nullptr;
    bool inBatch_ = false;
    std::int64_t backfillDone_ = 0;
    std::int64_t backfillUntil_ = 0;

    mutable std::recursive_mutex readMutex_;
    sqlite3* readDb_ = nullptr;  // == db_ for an in-memory database
};

} // namespace datasearch::core
