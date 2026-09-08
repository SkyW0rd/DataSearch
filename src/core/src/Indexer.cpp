#include "datasearch/core/Indexer.h"

#include "datasearch/core/Utf8.h"

#include <algorithm>
#include <cctype>
#include <mutex>

namespace datasearch::core {

namespace {

std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::size_t defaultThreadCount() {
    const unsigned hw = std::thread::hardware_concurrency();
    const std::size_t half = (hw == 0) ? 1 : static_cast<std::size_t>(hw) / 2;
    return std::max<std::size_t>(1, std::min<std::size_t>(4, half));
}

} // namespace

Indexer::Indexer(IndexStorage& storage, ScanOptions scanOptions, ExtractionOptions extractionOptions,
                  IndexerOptions indexerOptions)
    : storage_(storage),
      scanOptions_(std::move(scanOptions)),
      extractionOptions_(extractionOptions),
      indexerOptions_(std::move(indexerOptions)) {}

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
    storage_.beginBatch();

    std::atomic<std::size_t> nextRootIndex{0};
    std::uint64_t filesIndexed = 0;  // guarded by storageMutex
    std::mutex storageMutex;

    const std::size_t threadCount = std::max<std::size_t>(
        1, std::min(roots.empty() ? std::size_t{1} : roots.size(),
                     indexerOptions_.threadCount == 0 ? defaultThreadCount() : indexerOptions_.threadCount));

    auto worker = [&]() {
        if (indexerOptions_.onWorkerThreadStart) indexerOptions_.onWorkerThreadStart();

        while (!cancelled_.load(std::memory_order_relaxed)) {
            const std::size_t idx = nextRootIndex.fetch_add(1, std::memory_order_relaxed);
            if (idx >= roots.size()) break;

            FileScanner::scan(
                roots[idx], scanOptions_,
                [&](const FileRecord& record) {
                    std::string content;
                    const std::string ext = toLowerAscii(record.extension);
                    if (ContentExtractor::isSupportedExtension(ext)) {
                        if (auto extracted =
                                ContentExtractor::extract(pathFromUtf8(record.path), ext, extractionOptions_)) {
                            content = std::move(*extracted);
                        }
                    }

                    std::uint64_t countSnapshot;
                    {
                        std::lock_guard<std::mutex> lock(storageMutex);
                        storage_.upsertFile(record, content);
                        countSnapshot = ++filesIndexed;
                        if (countSnapshot % kBatchSize == 0) {
                            storage_.commitBatch();
                            storage_.beginBatch();
                        }
                    }
                    if (onProgress) onProgress(IndexProgress{countSnapshot, record.path});
                },
                &cancelled_);
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (std::size_t i = 0; i < threadCount; ++i) workers.emplace_back(worker);
    for (auto& t : workers) t.join();

    {
        std::lock_guard<std::mutex> lock(storageMutex);
        storage_.commitBatch();
    }

    const bool wasCancelled = cancelled_.load(std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    if (onComplete) {
        onComplete(wasCancelled);
    }
}

} // namespace datasearch::core
