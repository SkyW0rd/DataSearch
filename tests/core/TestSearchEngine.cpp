#include "TestFramework.h"

#include "datasearch/core/SearchEngine.h"

using datasearch::core::FileRecord;
using datasearch::core::IndexStorage;
using datasearch::core::SearchEngine;
using datasearch::core::SearchQuery;
using datasearch::core::SortField;
using datasearch::core::SortOrder;

namespace {

FileRecord makeRecord(const std::string& path, const std::string& name) {
    FileRecord r;
    r.path = path;
    r.name = name;
    r.size = 1;
    r.modifiedTime = 1;
    return r;
}

} // namespace

void runSearchEngineTests() {
    IndexStorage diskC(":memory:");
    IndexStorage diskD(":memory:");

    diskC.beginBatch();
    diskC.upsertFile(makeRecord("C:/alpha.txt", "alpha.txt"));
    diskC.upsertFile(makeRecord("C:/charlie.txt", "charlie.txt"));
    diskC.commitBatch();

    diskD.beginBatch();
    diskD.upsertFile(makeRecord("D:/bravo.txt", "bravo.txt"));
    diskD.upsertFile(makeRecord("D:/delta.txt", "delta.txt"));
    diskD.commitBatch();

    SearchEngine engine({&diskC, &diskD});

    // Merged, sorted by name across both sources: alpha, bravo, charlie, delta.
    SearchQuery q;
    q.namePattern = "";
    q.sortField = SortField::Name;
    q.sortOrder = SortOrder::Ascending;
    q.limit = 100;
    q.offset = 0;
    auto all = engine.search(q);
    DS_CHECK_EQ(all.size(), std::size_t{4});
    DS_CHECK_EQ(all[0].name, std::string("alpha.txt"));
    DS_CHECK_EQ(all[1].name, std::string("bravo.txt"));
    DS_CHECK_EQ(all[2].name, std::string("charlie.txt"));
    DS_CHECK_EQ(all[3].name, std::string("delta.txt"));

    // Global pagination must hold across sources, not just within one.
    SearchQuery page;
    page.namePattern = "";
    page.sortField = SortField::Name;
    page.sortOrder = SortOrder::Ascending;
    page.limit = 2;
    page.offset = 1;
    auto middle = engine.search(page);
    DS_CHECK_EQ(middle.size(), std::size_t{2});
    DS_CHECK_EQ(middle[0].name, std::string("bravo.txt"));
    DS_CHECK_EQ(middle[1].name, std::string("charlie.txt"));
}
