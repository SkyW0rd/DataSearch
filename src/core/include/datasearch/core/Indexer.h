#pragma once

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

// Runs a full scan of one or more roots on a background thread and writes the
// results into `storage`, batching writes into transactions for throughput
// (ТЗ FR-4: индексация не блокирует UI). One Indexer runs one scan at a time;
// call join() (or destroy the Indexer) before starting another.
class Indexer {
public:
    explicit Indexer(IndexStorage& storage, ScanOptions options = {});
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
    ScanOptions options_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;

    void run(std::vector<std::filesystem::path> roots,
             ProgressCallback onProgress,
             CompletionCallback onComplete);
};

} // namespace datasearch::core
