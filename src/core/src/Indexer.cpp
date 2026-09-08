#include "datasearch/core/Indexer.h"

#include "datasearch/core/Utf8.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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

struct FileStat {
    std::uint64_t size = 0;
    std::int64_t modifiedTime = 0;
};

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
    worker_ = std::thread(&Indexer::runInternal, this, std::move(roots), std::move(onProgress),
                           std::move(onComplete), /*reconcileMode=*/false);
}

void Indexer::startReconcile(std::vector<std::filesystem::path> roots,
                              ProgressCallback onProgress,
                              CompletionCallback onComplete) {
    join();
    cancelled_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    worker_ = std::thread(&Indexer::runInternal, this, std::move(roots), std::move(onProgress),
                           std::move(onComplete), /*reconcileMode=*/true);
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

void Indexer::runInternal(std::vector<std::filesystem::path> roots,
                           ProgressCallback onProgress,
                           CompletionCallback onComplete,
                           bool reconcileMode) {
    // IndexStorage serializes its own access internally, so concurrent calls
    // from this method's worker threads (and from anything else touching the
    // same instance, e.g. a live file-watcher update) are safe without an
    // external mutex here. `visitedPathsMutex` below guards only this
    // function's own local bookkeeping set, which IndexStorage knows nothing
    // about.

    // Reconciliation (ТЗ п.13.2): snapshot what's already indexed *before*
    // scanning, so unchanged files can be skipped without touching the DB,
    // and so anything indexed-but-missing-on-disk can be found afterwards.
    std::unordered_map<std::string, FileStat> existingByPath;
    std::unordered_set<std::string> visitedPaths;
    std::mutex visitedPathsMutex;
    if (reconcileMode) {
        for (const auto& record : storage_.allRecords()) {
            existingByPath[record.path] = FileStat{record.size, record.modifiedTime};
        }
    }

    storage_.beginBatch();

    std::atomic<std::size_t> nextRootIndex{0};
    std::atomic<std::uint64_t> filesIndexed{0};

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
                    if (reconcileMode) {
                        {
                            std::lock_guard<std::mutex> lock(visitedPathsMutex);
                            visitedPaths.insert(record.path);
                        }
                        const auto it = existingByPath.find(record.path);
                        if (it != existingByPath.end() && it->second.size == record.size &&
                            it->second.modifiedTime == record.modifiedTime) {
                            return;  // unchanged: nothing to do (ТЗ п.13.2)
                        }
                    }

                    std::string content;
                    const std::string ext = toLowerAscii(record.extension);
                    if (ContentExtractor::isSupportedExtension(ext)) {
                        if (auto extracted =
                                ContentExtractor::extract(pathFromUtf8(record.path), ext, extractionOptions_)) {
                            content = std::move(*extracted);
                        }
                    }

                    storage_.upsertFile(record, content);
                    const std::uint64_t countSnapshot = filesIndexed.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (countSnapshot % kBatchSize == 0) {
                        storage_.commitBatch();
                        storage_.beginBatch();
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

    if (reconcileMode && !cancelled_.load(std::memory_order_relaxed)) {
        // Anything still indexed but never visited on this pass no longer
        // exists on disk (ТЗ п.13.2: "путь есть в индексе, но физически
        // отсутствует на диске -> запись удаляется").
        for (const auto& [path, stat] : existingByPath) {
            (void)stat;
            if (visitedPaths.find(path) == visitedPaths.end()) {
                storage_.removeFile(path);
            }
        }
    }

    storage_.commitBatch();

    const bool wasCancelled = cancelled_.load(std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    if (onComplete) {
        onComplete(wasCancelled);
    }
}

} // namespace datasearch::core
