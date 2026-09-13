#include "TestFramework.h"

#include "datasearch/core/Indexer.h"
#include "datasearch/core/IndexStorage.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

using namespace datasearch::core;

namespace {

std::filesystem::path makeScratchDir() {
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / ("datasearch_reconcile_test_" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    return dir;
}

void writeFileAt(const std::filesystem::path& path, const std::string& content,
                  std::filesystem::file_time_type mtime) {
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary);
        out << content;
    }
    std::filesystem::last_write_time(path, mtime);
}

} // namespace

void runIndexerReconcileTests() {
    const auto root = makeScratchDir();
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{root};

    const auto t0 = std::filesystem::file_time_type::clock::now() - std::chrono::hours(2);
    const auto t1 = t0 + std::chrono::hours(1);

    writeFileAt(root / "a.txt", "hello", t0);
    writeFileAt(root / "b.txt", "world", t0);

    IndexStorage storage(":memory:");
    Indexer indexer(storage);
    indexer.start({root});
    indexer.join();
    DS_CHECK_EQ(storage.fileCount(), std::uint64_t{2});

    // Simulate what happened while nobody was watching: a.txt changed,
    // b.txt was deleted, c.txt is brand new.
    writeFileAt(root / "a.txt", "hello updated", t1);
    std::filesystem::remove(root / "b.txt");
    writeFileAt(root / "c.txt", "new file", t1);

    indexer.startReconcile({root});
    indexer.join();

    DS_CHECK_EQ(storage.fileCount(), std::uint64_t{2});
    DS_CHECK_EQ(indexer.status().filesWritten, std::uint64_t{2});  // a.txt changed, c.txt new
    DS_CHECK_EQ(indexer.status().filesRemoved, std::uint64_t{1});  // b.txt gone

    auto all = storage.allRecords();
    bool hasA = false, hasB = false, hasC = false;
    for (const auto& r : all) {
        if (r.path.find("a.txt") != std::string::npos) hasA = true;
        if (r.path.find("b.txt") != std::string::npos) hasB = true;
        if (r.path.find("c.txt") != std::string::npos) hasC = true;
    }
    DS_CHECK(hasA);
    DS_CHECK(!hasB);
    DS_CHECK(hasC);

    // a.txt's content must have been re-extracted (not skipped as "unchanged").
    SearchQuery q;
    q.namePattern = "updated";
    auto results = storage.search(q);
    DS_CHECK_EQ(results.size(), std::size_t{1});
    DS_CHECK(results[0].path.find("a.txt") != std::string::npos);

    // A second reconcile with nothing changed on disk must be a stable no-op.
    indexer.startReconcile({root});
    indexer.join();
    DS_CHECK_EQ(storage.fileCount(), std::uint64_t{2});
    DS_CHECK_EQ(indexer.status().filesWritten, std::uint64_t{0});
    DS_CHECK_EQ(indexer.status().filesRemoved, std::uint64_t{0});

#if !defined(_WIN32)
    // A folder that can't be read mid-walk must neither cut the rest of the
    // walk short (files after it went unseen) nor make a reconcile drop what
    // was indexed inside it as "deleted". Both used to happen, and with
    // intermittent errors every start removed and re-added tens of thousands
    // of files.
    {
        const auto dir = root / "walk_case";
        writeFileAt(dir / "a_first" / "1.txt", "one", t0);
        writeFileAt(dir / "m_locked" / "2.txt", "two", t0);
        writeFileAt(dir / "z_last" / "3.txt", "three", t0);

        IndexStorage walkStorage(":memory:");
        Indexer walkIndexer(walkStorage);
        walkIndexer.start({dir});
        walkIndexer.join();
        DS_CHECK_EQ(walkStorage.fileCount(), std::uint64_t{3});

        std::filesystem::permissions(dir / "m_locked", std::filesystem::perms::none);
        std::error_code probe;
        std::filesystem::directory_iterator(dir / "m_locked", probe);
        if (probe) {  // not when running as root, where permissions don't bite
            writeFileAt(dir / "z_last" / "4.txt", "four", t1);
            walkIndexer.startReconcile({dir});
            walkIndexer.join();
            DS_CHECK_EQ(walkStorage.fileCount(), std::uint64_t{4});  // 2.txt kept, 4.txt found
            DS_CHECK_EQ(walkIndexer.status().unreadableDirs, std::uint64_t{1});
            DS_CHECK_EQ(walkIndexer.status().filesWritten, std::uint64_t{1});
        }
        std::filesystem::permissions(dir / "m_locked", std::filesystem::perms::owner_all);
    }
#endif
}

void runIndexerUnavailableRootTests() {
    const auto root = makeScratchDir();
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{root};

    writeFileAt(root / "a.txt", "hello", std::filesystem::file_time_type::clock::now());
    writeFileAt(root / "b.txt", "world", std::filesystem::file_time_type::clock::now());

    IndexStorage storage(":memory:");
    Indexer indexer(storage);
    indexer.start({root});
    indexer.join();
    DS_CHECK_EQ(storage.fileCount(), std::uint64_t{2});

    // Simulate the source becoming unreachable (ТЗ п.11.4, e.g. a
    // disconnected network drive) by reconciling against a path that
    // doesn't currently resolve to a directory.
    bool unavailableReported = false;
    IndexerOptions options;
    options.onRootUnavailable = [&](const std::filesystem::path&) { unavailableReported = true; };
    Indexer reconciler(storage, {}, {}, options);
    reconciler.startReconcile({root / "does_not_exist_anymore"});
    reconciler.join();

    DS_CHECK(unavailableReported);
    // The existing index must be left untouched — not wiped just because the
    // source looked unreachable/empty this time.
    DS_CHECK_EQ(storage.fileCount(), std::uint64_t{2});
    auto all = storage.allRecords();
    DS_CHECK_EQ(all.size(), std::size_t{2});
}

// A file whose text can't be read is indexed by name and listed as a
// problem file; startup checks skip it while it stays the same, and once it
// changes it's read again and drops off the list.
void runProblemFilesTests() {
    const auto root = makeScratchDir();
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{root};

    const auto t0 = std::filesystem::file_time_type::clock::now() - std::chrono::hours(2);
    const auto t1 = t0 + std::chrono::hours(1);
    writeFileAt(root / "good.txt", "ordinary words", t0);
    writeFileAt(root / "report broken.docx", std::string(4000, 'z'), t0);
    writeFileAt(root / "huge notes.txt", std::string(200, 'q'), t0);

    ExtractionOptions limits;
    limits.maxBytes = 100;  // huge notes.txt is over it
    IndexStorage storage(":memory:");
    std::vector<std::string> writeErrors;
    IndexerOptions options;
    options.onFileError = [&](const std::string& path, const std::string&) { writeErrors.push_back(path); };
    Indexer indexer(storage, ScanOptions{}, limits, options);
    indexer.start({root});
    indexer.join();

    DS_CHECK_EQ(storage.fileCount(), std::uint64_t{3});  // all three, problem files included
    DS_CHECK_EQ(indexer.status().filesFailed, std::uint64_t{2});
    DS_CHECK(writeErrors.empty());  // not writing errors: the list covers them
    {
        const auto problems = storage.problemFiles();
        DS_CHECK_EQ(problems.size(), std::size_t{2});
        DS_CHECK(problems[0].path.find("huge notes.txt") != std::string::npos);
        DS_CHECK(problems[0].problem == ExtractionProblem::TooLarge);
        DS_CHECK(problems[1].path.find("report broken.docx") != std::string::npos);
        DS_CHECK(problems[1].problem == ExtractionProblem::Damaged);
        DS_CHECK_EQ(problems[1].size, std::uint64_t{4000});
        DS_CHECK(problems[1].seenAt > 0);
    }
    // Findable by name.
    SearchQuery byName;
    byName.namePattern = "broken";
    byName.limit = 10;
    DS_CHECK_EQ(storage.search(byName).size(), std::size_t{1});

    // Startup checks leave them alone while they're unchanged.
    for (int run = 0; run < 2; ++run) {
        indexer.startReconcile({root});
        indexer.join();
        DS_CHECK_EQ(indexer.status().filesWritten, std::uint64_t{0});
        DS_CHECK_EQ(indexer.status().filesFailed, std::uint64_t{0});
        DS_CHECK_EQ(storage.problemFiles().size(), std::size_t{2});
    }

    // Once it's fixed (changed on disk), it's read again and off the list.
    writeFileAt(root / "huge notes.txt", "short now", t1);
    indexer.startReconcile({root});
    indexer.join();
    DS_CHECK_EQ(indexer.status().filesWritten, std::uint64_t{1});
    {
        const auto problems = storage.problemFiles();
        DS_CHECK_EQ(problems.size(), std::size_t{1});
        DS_CHECK(problems[0].path.find("report broken.docx") != std::string::npos);
    }
    SearchQuery byText;
    byText.namePattern = "short";
    byText.limit = 10;
    DS_CHECK_EQ(storage.search(byText).size(), std::size_t{1});

    // A deleted problem file leaves the list too.
    std::filesystem::remove(root / "report broken.docx");
    indexer.startReconcile({root});
    indexer.join();
    DS_CHECK_EQ(indexer.status().filesRemoved, std::uint64_t{1});
    DS_CHECK(storage.problemFiles().empty());
}
