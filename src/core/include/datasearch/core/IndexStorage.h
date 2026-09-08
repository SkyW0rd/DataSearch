#pragma once

#include "datasearch/core/FileRecord.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;

namespace datasearch::core {

enum class SortField { Name, ModifiedTime, Size };
enum class SortOrder { Ascending, Descending };

struct SearchQuery {
    std::string namePattern;  // substring, matched case-insensitively (ТЗ FR-10)
    SortField sortField = SortField::Name;
    SortOrder sortOrder = SortOrder::Ascending;
    int limit = 200;
    int offset = 0;
};

// One SQLite database = the index for a single source (one disk/folder root —
// ТЗ п.11.2: "индекс каждого диска хранится отдельно"). Not thread-safe: callers
// that touch the same instance from multiple threads must serialize access
// themselves (Indexer does this by owning the write side exclusively while it runs).
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

    void upsertFile(const FileRecord& record);
    void removeFile(const std::string& path);

    // Full dump of indexed paths + their stored metadata — used for the
    // startup metadata-reconciliation pass (ТЗ п.13.2), not for interactive search.
    std::vector<FileRecord> allRecords() const;

    std::vector<FileRecord> search(const SearchQuery& query) const;

    std::uint64_t fileCount() const;

private:
    sqlite3* db_ = nullptr;
    bool inBatch_ = false;
};

} // namespace datasearch::core
