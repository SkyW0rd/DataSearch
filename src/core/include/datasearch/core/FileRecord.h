#pragma once

#include <cstdint>
#include <string>

namespace datasearch::core {

// One indexed file — the "document" in Elasticsearch terms (see ТЗ п.2).
struct FileRecord {
    std::string path;
    std::string name;
    std::string extension;
    std::uint64_t size = 0;
    std::int64_t createdTime = 0;   // seconds since Unix epoch, UTC; 0 if unknown
    std::int64_t modifiedTime = 0;  // seconds since Unix epoch, UTC

    // Highlighted excerpt around a content match (ТЗ FR-15); only populated
    // by IndexStorage::search()/SearchEngine::search() for a non-empty query,
    // empty otherwise.
    std::string snippet;

    // FTS5 bm25() score for this match (lower = more relevant); 0 for a
    // plain metadata browse with no query text. Used to merge/re-rank
    // results across several per-source indexes in SearchEngine.
    double relevanceScore = 0.0;
};

} // namespace datasearch::core
