#include "TestFramework.h"

#include "datasearch/core/Indexer.h"
#include "datasearch/core/IndexStorage.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

using namespace datasearch::core;

namespace {

std::filesystem::path makeScratchDir() {
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / ("datasearch_pause_test_" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    return dir;
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

} // namespace

void runIndexerPauseResumeTests() {
    const auto root = makeScratchDir();
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{root};

    for (int i = 0; i < 10; ++i) {
        writeFile(root / ("f" + std::to_string(i) + ".txt"), "content");
    }

    IndexStorage storage(":memory:");
    Indexer indexer(storage);

    // Pausing *before* start() means the very first file the worker thread
    // would visit blocks immediately — no race window where a few files
    // sneak through before the pause takes effect.
    indexer.pause();
    DS_CHECK(indexer.isPaused());
    indexer.start({root});

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    DS_CHECK_EQ(storage.fileCount(), std::uint64_t{0});
    DS_CHECK(indexer.isRunning());  // still alive, just blocked — not finished/cancelled
    {
        // Parked in the counting pass: the UI must be able to tell "paused"
        // (nothing in flight) apart from "pause requested, finishing a file".
        const IndexerStatus s = indexer.status();
        DS_CHECK(s.phase == IndexPhase::Counting);
        DS_CHECK(s.pauseRequested);
        DS_CHECK(s.paused);
    }

    indexer.resume();
    DS_CHECK(!indexer.isPaused());
    indexer.join();
    DS_CHECK_EQ(storage.fileCount(), std::uint64_t{10});
    {
        const IndexerStatus s = indexer.status();
        DS_CHECK(s.phase == IndexPhase::Finished);
        DS_CHECK(!s.cancelled);
        DS_CHECK(!s.paused);
        DS_CHECK_EQ(s.filesTotal, std::uint64_t{10});
        DS_CHECK_EQ(s.filesVisited, std::uint64_t{10});
        DS_CHECK_EQ(s.filesWritten, std::uint64_t{10});
        DS_CHECK(!s.currentInProgress);
    }

    // cancel() must wake a paused run rather than leaving it stuck forever.
    for (int i = 10; i < 20; ++i) {
        writeFile(root / ("g" + std::to_string(i) + ".txt"), "content");
    }
    indexer.pause();
    indexer.start({root});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    indexer.cancel();
    indexer.join();  // must not hang
    DS_CHECK(!indexer.isRunning());
    DS_CHECK(indexer.status().phase == IndexPhase::Finished);
    DS_CHECK(indexer.status().cancelled);

    // Pause/resume hammered from another thread while a run is in progress:
    // the run must neither wedge (a lost wake-up would leave it parked
    // forever) nor skip files.
    for (int i = 20; i < 300; ++i) {
        writeFile(root / ("h" + std::to_string(i) + ".txt"), "content");
    }
    IndexStorage storage2(":memory:");
    Indexer indexer2(storage2);
    indexer2.start({root});
    for (int i = 0; i < 200 && indexer2.isRunning(); ++i) {
        indexer2.pause();
        indexer2.resume();
    }
    indexer2.resume();
    indexer2.join();
    DS_CHECK_EQ(storage2.fileCount(), std::uint64_t{300});
    DS_CHECK_EQ(indexer2.status().filesVisited, std::uint64_t{300});

    // Heavy files are set aside and indexed last, exactly once — even when
    // the walk meets them first ("0_..." sorts ahead of every small file).
    {
        const auto dir = root / "heavy_case";
        for (int i = 0; i < 20; ++i) writeFile(dir / ("small" + std::to_string(i) + ".txt"), "small file");
        writeFile(dir / "0_heavy.txt", std::string(4096, 'x'));

        IndexStorage storage3(":memory:");
        IndexerOptions options;
        options.heavyFileThreshold = 1024;
        Indexer indexer3(storage3, {}, {}, options);
        std::vector<std::string> order;
        std::mutex orderMutex;
        indexer3.start({dir}, [&](const IndexProgress& p) {
            std::lock_guard<std::mutex> lock(orderMutex);
            order.push_back(p.currentPath);
        });
        indexer3.join();

        DS_CHECK_EQ(order.size(), std::size_t{21});
        DS_CHECK(!order.empty() && order.back().find("0_heavy.txt") != std::string::npos);
        DS_CHECK_EQ(std::count_if(order.begin(), order.end(),
                                  [](const std::string& p) { return p.find("0_heavy.txt") != std::string::npos; }),
                    std::ptrdiff_t{1});
        const IndexerStatus s = indexer3.status();
        DS_CHECK_EQ(s.heavyFound, std::uint64_t{1});
        DS_CHECK_EQ(s.heavyDone, std::uint64_t{1});
        DS_CHECK_EQ(s.heavyBytesTotal, std::uint64_t{4096});
        DS_CHECK(!s.processingHeavy);
        DS_CHECK_EQ(s.filesVisited, std::uint64_t{21});
        DS_CHECK_EQ(storage3.fileCount(), std::uint64_t{21});
    }
}
