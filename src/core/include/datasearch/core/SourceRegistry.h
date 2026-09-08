#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;

namespace datasearch::core {

// One indexed source (disk/folder root) the app already knows about, so it
// can be reopened instantly at startup instead of re-indexing from scratch
// (ТЗ п.13.1/FR-23). `dbPath` is that source's own IndexStorage file
// (ТЗ п.11.2: one SQLite index per source).
struct SourceEntry {
    std::string root;
    std::string dbPath;
    std::int64_t lastFullScan = 0;  // seconds since Unix epoch; 0 = never
};

// Small persistent table of known sources, separate from any one source's
// own index — this is metadata *about* the set of indexes, not an index
// itself. Backed by its own tiny SQLite database.
class SourceRegistry {
public:
    explicit SourceRegistry(const std::filesystem::path& registryDbPath);
    ~SourceRegistry();

    SourceRegistry(const SourceRegistry&) = delete;
    SourceRegistry& operator=(const SourceRegistry&) = delete;

    void upsert(const SourceEntry& entry);
    void remove(const std::string& root);
    void touchLastFullScan(const std::string& root, std::int64_t when);

    std::vector<SourceEntry> all() const;

private:
    sqlite3* db_ = nullptr;
};

} // namespace datasearch::core
