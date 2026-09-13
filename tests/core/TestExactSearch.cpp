#include "TestFramework.h"

#include "datasearch/core/Fts5RussianTokenizer.h"
#include "datasearch/core/IndexStorage.h"

#include <sqlite3.h>

#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace datasearch::core;

namespace {

FileRecord makeRecord(const std::string& path) {
    FileRecord r;
    r.path = path;
    r.name = std::filesystem::path(path).filename().string();
    r.extension = ".txt";
    r.size = 10;
    r.modifiedTime = 100;
    return r;
}

std::vector<std::string> paths(const std::vector<FileRecord>& records) {
    std::vector<std::string> out;
    for (const auto& r : records) out.push_back(r.path);
    return out;
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) {
        if (x == s) return true;
    }
    return false;
}

SearchQuery exactQuery(const std::string& text) {
    SearchQuery q;
    q.namePattern = text;
    q.exactWords = true;
    q.limit = 100;
    return q;
}

} // namespace

void runExactSearchTests() {
    IndexStorage storage(":memory:");
    storage.upsertFile(makeRecord("/d/a.txt"), "\xc2\xab\xd0\x9c\xd0\xb8\xd1\x85\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2 \xd0\x95\xd0\xb2\xd0\xb3\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb9\xc2\xbb \xd0\xbf\xd0\xbe\xd0\xb4\xd0\xbf\xd0\xb8\xd1\x81\xd0\xb0\xd0\xbb \xd0\xb5\xd1\x89\xd1\x91 \xd0\xbe\xd0\xb4\xd0\xb8\xd0\xbd \xd0\xb4\xd0\xbe\xd0\xb3\xd0\xbe\xd0\xb2\xd0\xbe\xd1\x80");
    storage.upsertFile(makeRecord("/d/b.txt"), "\xd0\x9c\xd0\xb8\xd1\x85\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2\xd0\xb0 \xd0\x95\xd0\xb2\xd0\xb3\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f \xd0\xb8 \xd0\x9c\xd0\xb8\xd1\x85\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xba\xd0\xbe \xd0\x95\xd0\xb2\xd0\xb3\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f");
    storage.upsertFile(makeRecord("/d/c.txt"), "\xd0\x9c\xd0\x98\xd0\xa5\xd0\x90\xd0\x99\xd0\x9b\xd0\x9e\xd0\x92 - \xd0\xb7\xd0\xb0\xd0\xb3\xd0\xbb\xd0\xb0\xd0\xb2\xd0\xbd\xd1\x8b\xd0\xbc\xd0\xb8 \xd0\xb1\xd1\x83\xd0\xba\xd0\xb2\xd0\xb0\xd0\xbc\xd0\xb8");

    // Whole words exactly as typed, whatever the case: not "Mikhailova" or
    // "Mikhailenko" (the report that prompted exact mode).
    {
        const auto found = paths(storage.search(exactQuery("\xd0\x9c\xd0\xb8\xd1\x85\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2")));
        DS_CHECK_EQ(found.size(), std::size_t{2});
        DS_CHECK(has(found, "/d/a.txt"));
        DS_CHECK(has(found, "/d/c.txt"));
        DS_CHECK_EQ(storage.countMatches(exactQuery("\xd0\xbc\xd0\xb8\xd1\x85\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2")), std::uint64_t{2});
    }
    // Several words: all of them present, anywhere in the file.
    {
        const auto found = paths(storage.search(exactQuery("\xd0\xb5\xd0\xb2\xd0\xb3\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f \xd0\xbc\xd0\xb8\xd1\x85\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2\xd0\xb0")));
        DS_CHECK_EQ(found.size(), std::size_t{1});
        DS_CHECK(has(found, "/d/b.txt"));
    }
    // "yo" and "ye" count as the same letter.
    DS_CHECK_EQ(storage.search(exactQuery("\xd0\xb5\xd1\x89\xd0\xb5")).size(), std::size_t{1});
    DS_CHECK_EQ(storage.search(exactQuery("\xd0\x95\xd0\xa9\xd0\x81")).size(), std::size_t{1});
    // No partial words in exact mode.
    DS_CHECK_EQ(storage.search(exactQuery("\xd0\x9c\xd0\xb8\xd1\x85\xd0\xb0\xd0\xb9\xd0\xbb")).size(), std::size_t{0});

    // Word-forms mode (the checkbox) still finds the inflected forms.
    {
        SearchQuery q;
        q.namePattern = "\xd0\x9c\xd0\xb8\xd1\x85\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2";
        q.limit = 100;
        const auto found = paths(storage.search(q));
        DS_CHECK(has(found, "/d/a.txt"));
        DS_CHECK(has(found, "/d/b.txt"));
    }

    // Excerpts on demand: in exact mode the exact word is highlighted, not
    // its other forms; in forms mode FTS5's own snippet() is used.
    {
        const std::string snip = storage.snippet("/d/a.txt", exactQuery("\xd0\xbc\xd0\xb8\xd1\x85\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2"));
        DS_CHECK(snip.find("[\xd0\x9c\xd0\xb8\xd1\x85\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2]") != std::string::npos);
        DS_CHECK(storage.snippet("/d/b.txt", exactQuery("\xd0\xbc\xd0\xb8\xd1\x85\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2")).find('[') == std::string::npos);
        SearchQuery forms;
        forms.namePattern = "\xd0\xb4\xd0\xbe\xd0\xb3\xd0\xbe\xd0\xb2\xd0\xbe\xd1\x80\xd1\x8b";
        DS_CHECK(storage.snippet("/d/a.txt", forms).find("[\xd0\xb4\xd0\xbe\xd0\xb3\xd0\xbe\xd0\xb2\xd0\xbe\xd1\x80]") != std::string::npos);
        SearchQuery lazy = exactQuery("\xd0\x9c\xd0\xb8\xd1\x85\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2");
        lazy.withSnippets = false;
        for (const auto& r : storage.search(lazy)) DS_CHECK(r.snippet.empty());
    }

    // Changing and removing files keeps the exact index in step.
    storage.upsertFile(makeRecord("/d/a.txt"), "\xd1\x82\xd0\xb5\xd0\xbf\xd0\xb5\xd1\x80\xd1\x8c \xd0\xb7\xd0\xb4\xd0\xb5\xd1\x81\xd1\x8c \xd0\x9f\xd0\xb5\xd1\x82\xd1\x80\xd0\xbe\xd0\xb2");
    DS_CHECK_EQ(storage.search(exactQuery("\xd0\x9c\xd0\xb8\xd1\x85\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2")).size(), std::size_t{1});
    DS_CHECK_EQ(storage.search(exactQuery("\xd0\x9f\xd0\xb5\xd1\x82\xd1\x80\xd0\xbe\xd0\xb2")).size(), std::size_t{1});
    storage.removeFile("/d/c.txt");
    DS_CHECK_EQ(storage.search(exactQuery("\xd0\x9c\xd0\xb8\xd1\x85\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2")).size(), std::size_t{0});
    DS_CHECK_EQ(storage.search(exactQuery("\xd0\xb7\xd0\xb0\xd0\xb3\xd0\xbb\xd0\xb0\xd0\xb2\xd0\xbd\xd1\x8b\xd0\xbc\xd0\xb8")).size(), std::size_t{0});
}

// An index built before the exact-word index existed gets it filled in from
// the text it already stores, without touching the files themselves.
void runExactBackfillTests() {
    std::random_device rd;
    const auto dbPath =
        std::filesystem::temp_directory_path() / ("datasearch_backfill_" + std::to_string(rd()) + ".sqlite3");
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() {
            std::error_code ec;
            for (const char* suffix : {"", "-wal", "-shm"}) std::filesystem::remove(p.string() + suffix, ec);
        }
    } cleanup{dbPath};

    {
        IndexStorage storage(dbPath);
        for (int i = 0; i < 25; ++i) {
            storage.upsertFile(makeRecord("/old/" + std::to_string(i) + ".txt"),
                               "\xd0\xa1\xd0\xb8\xd0\xb4\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb2 \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb \xd0\xbd\xd0\xbe\xd0\xbc\xd0\xb5\xd1\x80 " + std::to_string(i));
        }
    }
    {
        // Turn it into an old-format index: no exact-word table.
        sqlite3* db = nullptr;
        sqlite3_open(dbPath.string().c_str(), &db);
        registerRussianFts5Tokenizer(db);
        sqlite3_exec(db, "DROP TABLE files_exact; DELETE FROM meta;", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }

    IndexStorage storage(dbPath);
    DS_CHECK_EQ(storage.exactBackfillRemaining(), std::uint64_t{25});
    DS_CHECK_EQ(storage.search(exactQuery("\xd0\xa1\xd0\xb8\xd0\xb4\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb2")).size(), std::size_t{0});  // not filled in yet

    // Writes while the backfill is under way: an old file changed, one
    // removed, one brand new. None may corrupt the contentless index.
    storage.upsertFile(makeRecord("/old/3.txt"), "\xd0\x9a\xd1\x83\xd0\xb7\xd0\xbd\xd0\xb5\xd1\x86\xd0\xbe\xd0\xb2 \xd0\xb2\xd0\xbc\xd0\xb5\xd1\x81\xd1\x82\xd0\xbe \xd0\xbf\xd1\x80\xd0\xb5\xd0\xb6\xd0\xbd\xd0\xb5\xd0\xb3\xd0\xbe");
    storage.removeFile("/old/4.txt");
    storage.upsertFile(makeRecord("/new/n.txt"), "\xd0\xa1\xd0\xb8\xd0\xb4\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb2 \xd0\xbd\xd0\xbe\xd0\xb2\xd1\x8b\xd0\xb9");

    std::uint64_t filled = 0;
    while (const std::uint64_t n = storage.backfillExactIndex(10)) filled += n;
    DS_CHECK_EQ(filled, std::uint64_t{24});
    DS_CHECK_EQ(storage.exactBackfillRemaining(), std::uint64_t{0});

    DS_CHECK_EQ(storage.search(exactQuery("\xd0\xa1\xd0\xb8\xd0\xb4\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb2")).size(), std::size_t{24});  // 25 - changed - removed + new
    DS_CHECK_EQ(storage.search(exactQuery("\xd0\x9a\xd1\x83\xd0\xb7\xd0\xbd\xd0\xb5\xd1\x86\xd0\xbe\xd0\xb2")).size(), std::size_t{1});
    DS_CHECK_EQ(storage.countMatches(exactQuery("\xd0\xbd\xd0\xbe\xd0\xbc\xd0\xb5\xd1\x80")), std::uint64_t{23});

    // Later edits to backfilled files work as usual.
    storage.upsertFile(makeRecord("/old/5.txt"), "\xd0\xa1\xd0\xbc\xd0\xb8\xd1\x80\xd0\xbd\xd0\xbe\xd0\xb2");
    DS_CHECK_EQ(storage.search(exactQuery("\xd0\xa1\xd0\xb8\xd0\xb4\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb2")).size(), std::size_t{23});
    DS_CHECK_EQ(storage.search(exactQuery("\xd0\xa1\xd0\xbc\xd0\xb8\xd1\x80\xd0\xbd\xd0\xbe\xd0\xb2")).size(), std::size_t{1});
}
