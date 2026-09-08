#include "TestFramework.h"

#include "datasearch/core/IndexStorage.h"

#include <filesystem>

using datasearch::core::FileRecord;
using datasearch::core::IndexStorage;
using datasearch::core::SearchQuery;
using datasearch::core::SortField;
using datasearch::core::SortOrder;

namespace {

FileRecord makeRecord(const std::string& path, const std::string& name, std::uint64_t size,
                       std::int64_t modifiedTime) {
    FileRecord r;
    r.path = path;
    r.name = name;
    r.extension = std::filesystem::path(name).extension().string();
    r.size = size;
    r.createdTime = modifiedTime;
    r.modifiedTime = modifiedTime;
    return r;
}

} // namespace

void runIndexStorageTests() {
    IndexStorage storage(":memory:");

    storage.beginBatch();
    storage.upsertFile(makeRecord("/d/Practicum1.docx", "Practicum1.docx", 100, 1000));
    storage.upsertFile(makeRecord("/d/report.pdf", "report.pdf", 200, 2000));
    storage.upsertFile(makeRecord("/d/sub/practicum2.txt", "practicum2.txt", 50, 3000));
    storage.upsertFile(makeRecord("/d/notes.txt", "notes.txt", 10, 4000));
    storage.commitBatch();

    DS_CHECK_EQ(storage.fileCount(), std::uint64_t{4});

    // Case-insensitive substring match (ТЗ FR-10).
    SearchQuery q;
    q.namePattern = "practicum";
    q.sortField = SortField::Name;
    q.sortOrder = SortOrder::Ascending;
    auto results = storage.search(q);
    DS_CHECK_EQ(results.size(), std::size_t{2});
    DS_CHECK_EQ(results[0].name, std::string("Practicum1.docx"));
    DS_CHECK_EQ(results[1].name, std::string("practicum2.txt"));

    // Sort by size descending.
    SearchQuery qAll;
    qAll.namePattern = "";
    qAll.sortField = SortField::Size;
    qAll.sortOrder = SortOrder::Descending;
    auto bySize = storage.search(qAll);
    DS_CHECK_EQ(bySize.size(), std::size_t{4});
    DS_CHECK_EQ(bySize.front().name, std::string("report.pdf"));
    DS_CHECK_EQ(bySize.back().name, std::string("notes.txt"));

    // Upsert on an existing path updates in place rather than duplicating.
    storage.upsertFile(makeRecord("/d/notes.txt", "notes.txt", 999, 5000));
    DS_CHECK_EQ(storage.fileCount(), std::uint64_t{4});
    auto updated = storage.search(SearchQuery{"notes", SortField::Name, SortOrder::Ascending, 10, 0});
    DS_CHECK_EQ(updated.size(), std::size_t{1});
    DS_CHECK_EQ(updated[0].size, std::uint64_t{999});

    storage.removeFile("/d/report.pdf");
    DS_CHECK_EQ(storage.fileCount(), std::uint64_t{3});

    auto all = storage.allRecords();
    DS_CHECK_EQ(all.size(), std::size_t{3});

    // Pagination.
    SearchQuery page;
    page.namePattern = "";
    page.sortField = SortField::Name;
    page.limit = 1;
    page.offset = 1;
    auto paged = storage.search(page);
    DS_CHECK_EQ(paged.size(), std::size_t{1});
}
