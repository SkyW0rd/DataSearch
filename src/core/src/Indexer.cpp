#include "datasearch/core/Indexer.h"

namespace datasearch::core {

Indexer::Indexer(IndexStorage& storage, ScanOptions options)
    : storage_(storage), options_(std::move(options)) {}

Indexer::~Indexer() {
    cancel();
    join();
}

void Indexer::start(std::vector<std::filesystem::path> roots,
                     ProgressCallback onProgress,
                     CompletionCallback onComplete) {
    join();  // make sure a previous run isn't still in flight
    cancelled_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    worker_ = std::thread(&Indexer::run, this, std::move(roots), std::move(onProgress),
                           std::move(onComplete));
}

void Indexer::cancel() {
    cancelled_.store(true, std::memory_order_relaxed);
}

void Indexer::join() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool Indexer::isRunning() const {
    return running_.load(std::memory_order_relaxed);
}

void Indexer::run(std::vector<std::filesystem::path> roots,
                   ProgressCallback onProgress,
                   CompletionCallback onComplete) {
    std::uint64_t filesIndexed = 0;
    storage_.beginBatch();

    for (const auto& root : roots) {
        if (cancelled_.load(std::memory_order_relaxed)) break;

        FileScanner::scan(
            root, options_,
            [&](const FileRecord& record) {
                storage_.upsertFile(record);
                ++filesIndexed;

                if (filesIndexed % kBatchSize == 0) {
                    storage_.commitBatch();
                    storage_.beginBatch();
                }
                if (onProgress) {
                    onProgress(IndexProgress{filesIndexed, record.path});
                }
            },
            &cancelled_);
    }

    storage_.commitBatch();

    const bool wasCancelled = cancelled_.load(std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    if (onComplete) {
        onComplete(wasCancelled);
    }
}

} // namespace datasearch::core
