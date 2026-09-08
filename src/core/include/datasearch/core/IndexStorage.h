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
    // Substring/prefix match against name+content (FTS5 MATCH, ТЗ FR-10/FR-11);
    // empty means "browse all files" (no ranking, just the plain file list).
    std::string namePattern;
    SortField sortField = SortField::Relevance;
    SortOrder sortOrder = SortOrder::Ascending;
    int limit = 200;
    int offset = 0;
};

// One SQLite database = the index for a single source (one disk/folder root —
// ТЗ п.11.2: "индекс каждого диска хранится отдельно"). Backed by an FTS5
// table (name + extracted content, ТЗ п.5.1 вариант A) using the custom
// 'ru_snowball' tokenizer (see Fts5RussianTokenizer.h) for Russian morphology
// (ТЗ п.11.3), with name weighted above content in BM25 ranking (ТЗ п.8).
// Safe to call from multiple threads concurrently: every public method locks
// an internal mutex (SQLite's own connection is already serialized-mode
// thread-safe — this mutex additionally protects this class's own C++-side
// batch-transaction bookkeeping, e.g. so a background reindex's explicit
// beginBatch()/commitBatch() can't be interleaved with a live file-watcher
// update or a search running on the GUI thread).
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
    // namePattern is non-empty; browsing all files leaves it empty.
    std::vector<FileRecord> search(const SearchQuery& query) const;

    std::uint64_t fileCount() const;

private:
    mutable std::recursive_mutex mutex_;
    sqlite3* db_ = nullptr;
    bool inBatch_ = false;
};

} // namespace datasearch::core
