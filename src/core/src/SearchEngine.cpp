#include "datasearch/core/SearchEngine.h"

#include "datasearch/core/NaturalOrder.h"

#include <algorithm>

namespace datasearch::core {

namespace {

// The same orders the "ds_path" collation gives each source's SQL query.
bool less(const FileRecord& a, const FileRecord& b, SortField field) {
    switch (field) {
        case SortField::ModifiedTime: return a.modifiedTime < b.modifiedTime;
        case SortField::Size: return a.size < b.size;
        case SortField::Name: return comparePaths(a.name, b.name) < 0;
        // bm25() is "smaller is more relevant" by FTS5 convention; comparing
        // raw scores across independently-ranked per-disk indexes is an
        // approximation (each index's term/document statistics differ), the
        // same simplification real distributed search engines make when
        // merging per-shard relevance scores.
        case SortField::Relevance: default: return a.relevanceScore < b.relevanceScore;
    }
}

} // namespace

SearchEngine::SearchEngine(std::vector<IndexStorage*> sources) : sources_(std::move(sources)) {}

std::vector<FileRecord> SearchEngine::search(const SearchQuery& query) const {
    if (sources_.empty()) return {};

    // Each source can only rank/limit within itself, so pull enough rows from
    // every source to guarantee a correct global [offset, offset+limit) window
    // once everything is merged and re-sorted.
    SearchQuery perSourceQuery = query;
    perSourceQuery.offset = 0;
    perSourceQuery.limit = query.offset + query.limit;

    std::vector<FileRecord> merged;
    for (IndexStorage* source : sources_) {
        auto partial = source->search(perSourceQuery);
        merged.insert(merged.end(), std::make_move_iterator(partial.begin()),
                       std::make_move_iterator(partial.end()));
    }

    const bool descending = query.sortOrder == SortOrder::Descending;
    if (query.sortField == SortField::Path) {
        // The direction is part of the order here: folders stay first.
        const PathOrder order{query.foldersFirst, descending};
        std::sort(merged.begin(), merged.end(),
                  [&](const FileRecord& a, const FileRecord& b) { return comparePaths(a.path, b.path, order) < 0; });
    } else {
        std::sort(merged.begin(), merged.end(), [&](const FileRecord& a, const FileRecord& b) {
            return descending ? less(b, a, query.sortField) : less(a, b, query.sortField);
        });
    }

    if (static_cast<std::size_t>(query.offset) >= merged.size()) return {};

    const auto begin = merged.begin() + query.offset;
    const auto end =
        merged.begin() + std::min(merged.size(), static_cast<std::size_t>(query.offset + query.limit));
    return std::vector<FileRecord>(begin, end);
}

std::uint64_t SearchEngine::countMatches(const SearchQuery& query) const {
    std::uint64_t total = 0;
    for (IndexStorage* source : sources_) total += source->countMatches(query);
    return total;
}

} // namespace datasearch::core
