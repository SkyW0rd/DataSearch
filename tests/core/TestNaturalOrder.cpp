#include "TestFramework.h"

#include "datasearch/core/IndexStorage.h"
#include "datasearch/core/NaturalOrder.h"
#include "datasearch/core/SearchEngine.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace datasearch::core;

namespace {

FileRecord makeRecord(const std::string& path, const std::string& ext = ".txt", std::int64_t modified = 100) {
    FileRecord r;
    r.path = path;
    r.name = path.substr(path.find_last_of("/\\") + 1);
    r.extension = ext;
    r.size = 10;
    r.modifiedTime = modified;
    return r;
}

std::vector<std::string> paths(const std::vector<FileRecord>& records) {
    std::vector<std::string> out;
    for (const auto& r : records) out.push_back(r.path);
    return out;
}

bool before(std::string_view a, std::string_view b) { return compareNatural(a, b) < 0 && compareNatural(b, a) > 0; }
bool pathBefore(std::string_view a, std::string_view b) { return comparePaths(a, b) < 0 && comparePaths(b, a) > 0; }

} // namespace

void runNaturalOrderTests() {
    // Case and yo/ye don't matter; Russian letters in alphabet order.
    DS_CHECK(before("\xd0\xb0", "\xd0\x91"));
    DS_CHECK(before("\xd0\x95\xd0\xb6\xd0\xb5\xd0\xb2\xd0\xb8\xd0\xba\xd0\xb0", "\xd1\x91\xd0\xbb\xd0\xba\xd0\xb0"));
    DS_CHECK(before("\xd1\x91\xd0\xbb\xd0\xba\xd0\xb0", "\xd0\x96\xd1\x83\xd0\xba"));
    DS_CHECK(before("apple", "Banana"));
    DS_CHECK(before("zebra", "\xd0\xb0\xd1\x80\xd0\xb1\xd1\x83\xd0\xb7"));  // Latin before Cyrillic, like Explorer
    // Only case differs: still a strict order, by bytes.
    DS_CHECK(compareNatural("\xd0\x9e\xd1\x82\xd1\x87\xd1\x91\xd1\x82", "\xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82") != 0);
    DS_CHECK_EQ(compareNatural("\xd0\x9e\xd1\x82\xd1\x87\xd1\x91\xd1\x82", "\xd0\x9e\xd1\x82\xd1\x87\xd1\x91\xd1\x82"), 0);

    // Numbers by value, also past a shared prefix.
    DS_CHECK(before("\xd0\x94\xd0\xbe\xd0\xb3\xd0\xbe\xd0\xb2\xd0\xbe\xd1\x80 2.docx", "\xd0\x94\xd0\xbe\xd0\xb3\xd0\xbe\xd0\xb2\xd0\xbe\xd1\x80 10.docx"));
    DS_CHECK(before("file9", "file10"));
    DS_CHECK(before("v1.9", "v1.10"));
    DS_CHECK(before("2023-12", "2024-01"));
    DS_CHECK(before("007", "8"));
    DS_CHECK(before("\xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb 19", "\xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb 100"));
    DS_CHECK(before("a1b", "a01c"));

    // Paths, compared folder by folder. Default, as in Explorer: in each
    // folder its subfolders first, then its own files; a folder named like
    // another plus " 2" comes after that other one.
    const std::vector<std::string> list = {
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95 2/a.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd0\x9f\xd0\xbe\xd0\xb4\xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0/b.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd1\x8f.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd0\xb0.docx",
        "/EcoLine/\xd0\x90\xd1\x80\xd1\x85\xd0\xb8\xd0\xb2/c.docx",
        "/EcoLine/\xd0\xb2\xd0\xb0\xd0\xb6\xd0\xbd\xd0\xbe\xd0\xb5.txt",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95.old/d.docx",
        "/Alpha/x.txt",
    };
    const std::vector<std::string> foldersFirst = {
        "/Alpha/x.txt",
        "/EcoLine/\xd0\x90\xd1\x80\xd1\x85\xd0\xb8\xd0\xb2/c.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd0\x9f\xd0\xbe\xd0\xb4\xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0/b.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd0\xb0.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd1\x8f.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95 2/a.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95.old/d.docx",
        "/EcoLine/\xd0\xb2\xd0\xb0\xd0\xb6\xd0\xbd\xd0\xbe\xd0\xb5.txt",
    };
    // From Ya to A at every level, subfolders still before files.
    const std::vector<std::string> foldersFirstDescending = {
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95.old/d.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95 2/a.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd0\x9f\xd0\xbe\xd0\xb4\xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0/b.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd1\x8f.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd0\xb0.docx",
        "/EcoLine/\xd0\x90\xd1\x80\xd1\x85\xd0\xb8\xd0\xb2/c.docx",
        "/EcoLine/\xd0\xb2\xd0\xb0\xd0\xb6\xd0\xbd\xd0\xbe\xd0\xb5.txt",
        "/Alpha/x.txt",
    };
    // Files of each folder before its subfolders.
    const std::vector<std::string> filesFirst = {
        "/Alpha/x.txt",
        "/EcoLine/\xd0\xb2\xd0\xb0\xd0\xb6\xd0\xbd\xd0\xbe\xd0\xb5.txt",
        "/EcoLine/\xd0\x90\xd1\x80\xd1\x85\xd0\xb8\xd0\xb2/c.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd0\xb0.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd1\x8f.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd0\x9f\xd0\xbe\xd0\xb4\xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0/b.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95 2/a.docx",
        "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95.old/d.docx",
    };
    auto sorted = [&](PathOrder order) {
        auto copy = list;
        std::sort(copy.begin(), copy.end(),
                  [order](const std::string& a, const std::string& b) { return comparePaths(a, b, order) < 0; });
        return copy;
    };
    DS_CHECK(sorted({}) == foldersFirst);
    DS_CHECK(sorted({true, true}) == foldersFirstDescending);
    DS_CHECK(sorted({false, false}) == filesFirst);
    // Descending keeps that grouping too.
    DS_CHECK(sorted({false, true}) == (std::vector<std::string>{
                                         "/EcoLine/\xd0\xb2\xd0\xb0\xd0\xb6\xd0\xbd\xd0\xbe\xd0\xb5.txt",
                                         "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95.old/d.docx",
                                         "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95 2/a.docx",
                                         "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd1\x8f.docx",
                                         "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd0\xb0.docx",
                                         "/EcoLine/\xd0\x92\xd0\x90\xd0\x96\xd0\x9d\xd0\x9e\xd0\x95/\xd0\x9f\xd0\xbe\xd0\xb4\xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0/b.docx",
                                         "/EcoLine/\xd0\x90\xd1\x80\xd1\x85\xd0\xb8\xd0\xb2/c.docx",
                                         "/Alpha/x.txt",
                                     }));

    // Windows separators and a file right in the drive's root.
    DS_CHECK(pathBefore("D:\\Work\\a.txt", "D:\\z.txt"));
    DS_CHECK(comparePaths("D:\\z.txt", "D:\\Work\\a.txt", {false, false}) < 0);
    DS_CHECK(pathBefore("D:\\Work\\\xd0\x9f\xd1\x80\xd0\xbe\xd0\xb5\xd0\xba\xd1\x82\\a.txt", "D:\\Work\\a.txt"));
    DS_CHECK(pathBefore("D:\\\xd0\x9f\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0 2\\x.txt", "D:\\\xd0\x9f\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0 10\\a.txt"));

    // Through SQLite: each source sorted by the collation, and the merge of
    // several sources in the same order.
    IndexStorage first(":memory:");
    IndexStorage second(":memory:");
    for (std::size_t i = 0; i < list.size(); ++i) (i % 2 == 0 ? first : second).upsertFile(makeRecord(list[i]), "\xd0\xbe\xd0\xb1\xd1\x89\xd0\xb5\xd0\xb5 \xd1\x81\xd0\xbb\xd0\xbe\xd0\xb2\xd0\xbe");
    SearchQuery byPath;
    byPath.namePattern = "\xd0\xbe\xd0\xb1\xd1\x89\xd0\xb5\xd0\xb5";
    byPath.sortField = SortField::Path;
    byPath.limit = 100;
    {
        const auto one = paths(first.search(byPath));
        DS_CHECK(std::is_sorted(one.begin(), one.end(),
                                [](const std::string& a, const std::string& b) { return comparePaths(a, b) < 0; }));
        DS_CHECK_EQ(one.size(), std::size_t{4});
    }
    SearchEngine engine({&first, &second});
    DS_CHECK(paths(engine.search(byPath)) == foldersFirst);
    byPath.sortOrder = SortOrder::Descending;
    DS_CHECK(paths(engine.search(byPath)) == foldersFirstDescending);
    byPath.foldersFirst = false;
    byPath.sortOrder = SortOrder::Ascending;
    DS_CHECK(paths(engine.search(byPath)) == filesFirst);
    // Browsing without a query, too.
    byPath.namePattern.clear();
    DS_CHECK(paths(engine.search(byPath)) == filesFirst);
    byPath.foldersFirst = true;
    byPath.sortOrder = SortOrder::Descending;
    DS_CHECK(paths(first.search(byPath)).front() == foldersFirstDescending.front());
    DS_CHECK(paths(engine.search(byPath)) == foldersFirstDescending);
    // By name, numbers by value.
    {
        IndexStorage storage(":memory:");
        storage.upsertFile(makeRecord("/b/\xd0\x94\xd0\xbe\xd0\xb3\xd0\xbe\xd0\xb2\xd0\xbe\xd1\x80 10.docx"));
        storage.upsertFile(makeRecord("/a/\xd0\xb4\xd0\xbe\xd0\xb3\xd0\xbe\xd0\xb2\xd0\xbe\xd1\x80 9.docx"));
        storage.upsertFile(makeRecord("/c/\xd0\x94\xd0\xbe\xd0\xb3\xd0\xbe\xd0\xb2\xd0\xbe\xd1\x80 100.docx"));
        SearchQuery byName;
        byName.sortField = SortField::Name;
        byName.limit = 10;
        DS_CHECK(paths(storage.search(byName)) ==
                 (std::vector<std::string>{"/a/\xd0\xb4\xd0\xbe\xd0\xb3\xd0\xbe\xd0\xb2\xd0\xbe\xd1\x80 9.docx", "/b/\xd0\x94\xd0\xbe\xd0\xb3\xd0\xbe\xd0\xb2\xd0\xbe\xd1\x80 10.docx", "/c/\xd0\x94\xd0\xbe\xd0\xb3\xd0\xbe\xd0\xb2\xd0\xbe\xd1\x80 100.docx"}));
    }
}

void runSearchFilterTests() {
    IndexStorage storage(":memory:");
    storage.upsertFile(makeRecord("/d/\xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82.docx", ".docx", 1000), "\xd0\xba\xd0\xb2\xd0\xb0\xd1\x80\xd1\x82\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd1\x8b\xd0\xb9 \xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82");
    storage.upsertFile(makeRecord("/d/\xd0\x9e\xd1\x82\xd1\x87\xd1\x91\xd1\x82.DOC", ".DOC", 5000), "\xd0\xb3\xd0\xbe\xd0\xb4\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb9 \xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82");
    storage.upsertFile(makeRecord("/d/\xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82.xlsx", ".xlsx", 9000), "\xd1\x82\xd0\xb0\xd0\xb1\xd0\xbb\xd0\xb8\xd1\x86\xd0\xb0 \xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82");
    storage.upsertFile(makeRecord("/d/\xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82.pdf", ".pdf", 20000), "\xd1\x81\xd0\xba\xd0\xb0\xd0\xbd \xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82");

    SearchQuery q;
    q.namePattern = "\xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82";
    q.exactWords = true;
    q.limit = 100;
    q.sortField = SortField::Path;
    DS_CHECK_EQ(storage.countMatches(q), std::uint64_t{4});

    // Type: any extension of the group, whatever its case on disk.
    q.extensions = {".doc", ".docx"};
    DS_CHECK(paths(storage.search(q)) == (std::vector<std::string>{"/d/\xd0\x9e\xd1\x82\xd1\x87\xd1\x91\xd1\x82.DOC", "/d/\xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82.docx"}));
    DS_CHECK_EQ(storage.countMatches(q), std::uint64_t{2});

    // Date: modified at or after the moment given.
    q.extensions.clear();
    q.modifiedSince = 9000;
    DS_CHECK(paths(storage.search(q)) == (std::vector<std::string>{"/d/\xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82.pdf", "/d/\xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82.xlsx"}));
    DS_CHECK_EQ(storage.countMatches(q), std::uint64_t{2});

    // Both, together with an ext: operator in the text.
    q.extensions = {".xlsx", ".pdf"};
    q.namePattern = "\xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82 ext:pdf";
    DS_CHECK(paths(storage.search(q)) == (std::vector<std::string>{"/d/\xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82.pdf"}));
    q.namePattern = "\xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82 ext:docx";
    DS_CHECK_EQ(storage.countMatches(q), std::uint64_t{0});

    // Filters apply when browsing without a query, and in word-forms mode.
    SearchQuery browse;
    browse.limit = 100;
    browse.extensions = {".pdf", ".xlsx"};
    DS_CHECK_EQ(storage.search(browse).size(), std::size_t{2});
    DS_CHECK_EQ(storage.countMatches(browse), std::uint64_t{2});
    SearchQuery forms;
    forms.namePattern = "\xd0\xbe\xd1\x82\xd1\x87\xd0\xb5\xd1\x82";
    forms.limit = 100;
    forms.modifiedSince = 4000;
    DS_CHECK_EQ(storage.countMatches(forms), std::uint64_t{3});
}
