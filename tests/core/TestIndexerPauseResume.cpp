#include "TestFramework.h"

#include "datasearch/core/Indexer.h"
#include "datasearch/core/IndexStorage.h"

#include <chrono>
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

    indexer.resume();
    DS_CHECK(!indexer.isPaused());
    indexer.join();
    DS_CHECK_EQ(storage.fileCount(), std::uint64_t{10});

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
}
