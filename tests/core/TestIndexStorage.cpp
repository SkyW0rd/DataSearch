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

void runIndexStorageContentSearchTests() {
    IndexStorage storage(":memory:");

    FileRecord doc;
    doc.path = "/d/instructions.txt";
    doc.name = "instructions.txt";
    doc.extension = ".txt";
    doc.size = 42;
    doc.modifiedTime = 111;

    // Content: "Инструкция к практикуму номер один" — a content-only match,
    // using a different inflected form of "практикум" ("практикуму", dative)
    // than the one searched for below ("практикума", genitive), to prove the
    // Russian stemmer is actually wired into the FTS5 tokenizer end-to-end,
    // not just unit-tested in isolation. Written as raw UTF-8 byte escapes
    // (rather than literal Cyrillic source characters) so this file's bytes
    // are plain ASCII regardless of the compiler's assumed source encoding.
    const std::string content =
        "\xd0\x98\xd0\xbd\xd1\x81\xd1\x82\xd1\x80\xd1\x83\xd0\xba\xd1\x86\xd0\xb8\xd1\x8f "
        "\xd0\xba \xd0\xbf\xd1\x80\xd0\xb0\xd0\xba\xd1\x82\xd0\xb8\xd0\xba\xd1\x83\xd0\xbc\xd1\x83 "
        "\xd0\xbd\xd0\xbe\xd0\xbc\xd0\xb5\xd1\x80 \xd0\xbe\xd0\xb4\xd0\xb8\xd0\xbd";
    storage.upsertFile(doc, content);

    FileRecord other;
    other.path = "/d/unrelated.txt";
    other.name = "unrelated.txt";
    other.extension = ".txt";
    other.size = 5;
    other.modifiedTime = 222;
    storage.upsertFile(other, "just some unrelated english text");

    // "практикума" (genitive) — must still match "практикуму" (dative) above.
    SearchQuery q;
    q.namePattern = "\xd0\xbf\xd1\x80\xd0\xb0\xd0\xba\xd1\x82\xd0\xb8\xd0\xba\xd1\x83\xd0\xbc\xd0\xb0";
    auto results = storage.search(q);
    DS_CHECK_EQ(results.size(), std::size_t{1});
    DS_CHECK_EQ(results[0].path, doc.path);
    DS_CHECK(!results[0].snippet.empty());

    // A plain English word search should not accidentally match the Russian doc.
    SearchQuery qEnglish;
    qEnglish.namePattern = "unrelated";
    auto englishResults = storage.search(qEnglish);
    DS_CHECK_EQ(englishResults.size(), std::size_t{1});
    DS_CHECK_EQ(englishResults[0].path, other.path);
}

void runIndexStorageQueryOperatorTests() {
    IndexStorage storage(":memory:");

    FileRecord report;
    report.path = "D:\\Work\\report.docx";
    report.name = "report.docx";
    report.extension = ".docx";
    report.size = 10;
    report.modifiedTime = 1;
    storage.upsertFile(report, "quarterly report draft");

    FileRecord finalReport;
    finalReport.path = "D:\\Work\\final.docx";
    finalReport.name = "final.docx";
    finalReport.extension = ".docx";
    finalReport.size = 20;
    finalReport.modifiedTime = 2;
    storage.upsertFile(finalReport, "quarterly report final version");

    FileRecord otherExt;
    otherExt.path = "D:\\Work\\report.pdf";
    otherExt.name = "report.pdf";
    otherExt.extension = ".pdf";
    otherExt.size = 30;
    otherExt.modifiedTime = 3;
    storage.upsertFile(otherExt, "quarterly report");

    FileRecord elsewhere;
    elsewhere.path = "D:\\Personal\\report.docx";
    elsewhere.name = "report.docx";
    elsewhere.extension = ".docx";
    elsewhere.size = 5;
    elsewhere.modifiedTime = 4;
    storage.upsertFile(elsewhere, "quarterly report");

    DS_CHECK_EQ(storage.fileCount(), std::uint64_t{4});

    // -exclusion: "quarterly" matches all four, "-draft" removes report.docx.
    {
        SearchQuery q;
        q.namePattern = "quarterly -draft";
        q.limit = 10;
        auto results = storage.search(q);
        DS_CHECK_EQ(results.size(), std::size_t{3});
        for (const auto& r : results) DS_CHECK(r.path != report.path);
    }

    // ext: filter combined with free text.
    {
        SearchQuery q;
        q.namePattern = "quarterly ext:pdf";
        q.limit = 10;
        auto results = storage.search(q);
        DS_CHECK_EQ(results.size(), std::size_t{1});
        DS_CHECK_EQ(results[0].path, otherExt.path);
    }

    // path: filter (substring, case-insensitive) combined with free text.
    {
        SearchQuery q;
        q.namePattern = "quarterly path:d:\\work";
        q.limit = 10;
        auto results = storage.search(q);
        DS_CHECK_EQ(results.size(), std::size_t{3});
        for (const auto& r : results) DS_CHECK(r.path != elsewhere.path);
    }

    // Exact phrase: "report final" matches finalReport's content, not the others.
    {
        SearchQuery q;
        q.namePattern = "\"report final\"";
        q.limit = 10;
        auto results = storage.search(q);
        DS_CHECK_EQ(results.size(), std::size_t{1});
        DS_CHECK_EQ(results[0].path, finalReport.path);
    }

    // ext: alone (no free text) still filters via the plain browse-all path.
    {
        SearchQuery q;
        q.namePattern = "ext:pdf";
        q.limit = 10;
        auto results = storage.search(q);
        DS_CHECK_EQ(results.size(), std::size_t{1});
        DS_CHECK_EQ(results[0].path, otherExt.path);
    }

    // -exclusion alone (no positive text) still filters via the browse-all path.
    {
        SearchQuery q;
        q.namePattern = "-draft";
        q.limit = 10;
        auto results = storage.search(q);
        DS_CHECK_EQ(results.size(), std::size_t{3});
        for (const auto& r : results) DS_CHECK(r.path != report.path);
    }
}
