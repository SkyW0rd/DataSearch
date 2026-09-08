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
