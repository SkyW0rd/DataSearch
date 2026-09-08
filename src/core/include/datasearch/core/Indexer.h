#pragma once

#include "datasearch/core/ContentExtractor.h"
#include "datasearch/core/FileScanner.h"
#include "datasearch/core/IndexStorage.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace datasearch::core {

struct IndexProgress {
    std::uint64_t filesIndexed = 0;
    std::string currentPath;
};

using ProgressCallback = std::function<void(const IndexProgress&)>;
using CompletionCallback = std::function<void(bool cancelled)>;

struct IndexerOptions {
    // Background-indexing parallelism cap (ТЗ п.12.3: "не более
    // количество_ядер/2, не менее 1 и не более 4"). 0 = pick that default.
    std::size_t threadCount = 0;

    // Called once at the start of each indexing worker thread, so the
    // platform layer can lower its OS thread priority (Windows:
    // THREAD_PRIORITY_BELOW_NORMAL, ТЗ п.12.3) without core depending on
    // platform directly.
    std::function<void()> onWorkerThreadStart;
};

// Runs a full scan of one or more roots on a background thread pool and
// writes the results (metadata + extracted content, ТЗ FR-3) into `storage`,
// batching writes into transactions for throughput (ТЗ FR-4: индексация не
// блокирует UI). Several roots are scanned in parallel across up to
// IndexerOptions::threadCount worker threads (ТЗ п.11.4: несколько дисков
// одновременно); storage writes are serialized internally regardless.
// One Indexer runs one scan at a time; call join() (or destroy the Indexer)
// before starting another.
class Indexer {
public:
    explicit Indexer(IndexStorage& storage, ScanOptions scanOptions = {},
                      ExtractionOptions extractionOptions = {}, IndexerOptions indexerOptions = {});
    ~Indexer();

    Indexer(const Indexer&) = delete;
    Indexer& operator=(const Indexer&) = delete;

    void start(std::vector<std::filesystem::path> roots,
               ProgressCallback onProgress = nullptr,
               CompletionCallback onComplete = nullptr);

    // Requests the running scan to stop at the next visited entry. Does not block.
    void cancel();

    // Blocks until the background thread finishes (a no-op if none is running).
    void join();

    bool isRunning() const;

private:
    static constexpr std::uint64_t kBatchSize = 500;

    IndexStorage& storage_;
    ScanOptions scanOptions_;
    ExtractionOptions extractionOptions_;
    IndexerOptions indexerOptions_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;

    void run(std::vector<std::filesystem::path> roots,
             ProgressCallback onProgress,
             CompletionCallback onComplete);
};

} // namespace datasearch::core
