#pragma once

#include "datasearch/core/IndexStorage.h"

#include <vector>

namespace datasearch::core {

// Searches across several per-source indexes (ТЗ п.11.4 — one SQLite index per
// disk/source, checkbox-selected in the UI) and merges the results into a
// single ranked/sorted list, as if querying one combined index.
class SearchEngine {
public:
    explicit SearchEngine(std::vector<IndexStorage*> sources);

    std::vector<FileRecord> search(const SearchQuery& query) const;

private:
    std::vector<IndexStorage*> sources_;
};

} // namespace datasearch::core
