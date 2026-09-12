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
    launch(std::move(roots), std::move(onProgress), std::move(onComplete), /*reconcileMode=*/false);
}

void Indexer::startReconcile(std::vector<std::filesystem::path> roots,
                              ProgressCallback onProgress,
                              CompletionCallback onComplete) {
    launch(std::move(roots), std::move(onProgress), std::move(onComplete), /*reconcileMode=*/true);
}

void Indexer::launch(std::vector<std::filesystem::path> roots, ProgressCallback onProgress,
                      CompletionCallback onComplete, bool reconcileMode) {
    join();  // make sure a previous run isn't still in flight
    cancelled_.store(false);
    reconcileMode_.store(reconcileMode);
    finishedCancelled_.store(false);
    totalIsEstimate_.store(false);
    filesTotal_.store(0);
    filesVisited_.store(0);
    filesWritten_.store(0);
    filesFailed_.store(0);
    activeWorkers_.store(0);
    parkedWorkers_.store(0);
    heavyFound_.store(0);
    heavyDone_.store(0);
    heavyBytesTotal_.store(0);
    heavyBytesDone_.store(0);
    heavyWorkTotal_.store(0);
    heavyWorkDone_.store(0);
    heavyActiveMs_.store(0);
    currentWork_.store(0);
    processingHeavy_.store(false);
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        currentPath_.clear();
        currentSize_ = 0;
        currentInProgress_ = false;
        startedAt_ = std::chrono::steady_clock::now();
        finishedAt_ = {};
    }
    // Set before the thread exists, so status() right after start() already
    // reports a live run rather than Idle/Finished.
    phase_.store(reconcileMode ? IndexPhase::Indexing : IndexPhase::Counting);
    running_.store(true);
    worker_ = std::thread(&Indexer::runInternal, this, std::move(roots), std::move(onProgress),
                           std::move(onComplete), reconcileMode);
}

// The flags are changed under pauseMutex_ (not just stored atomically) so a
// worker that has just seen "paused" and is about to block can't miss the
// wake-up: it holds the mutex from checking the flag until it's waiting.
void Indexer::cancel() {
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        cancelled_.store(true);
    }
    pauseCv_.notify_all();  // don't leave a paused run stuck waiting forever
}

void Indexer::pause() {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    paused_.store(true);
}

void Indexer::resume() {
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        paused_.store(false);
    }
    pauseCv_.notify_all();
}

bool Indexer::isPaused() const {
    return paused_.load(std::memory_order_relaxed);
}

void Indexer::waitWhilePaused() {
    if (!paused_.load(std::memory_order_relaxed)) return;
    std::unique_lock<std::mutex> lock(pauseMutex_);
    ++parkedWorkers_;
    pauseCv_.wait(lock, [this] {
        return !paused_.load(std::memory_order_relaxed) || cancelled_.load(std::memory_order_relaxed);
    });
    --parkedWorkers_;
}

void Indexer::beginFile(const std::string& path, std::uint64_t size) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    currentPath_ = path;
    currentSize_ = size;
    currentInProgress_ = true;
    currentStartedAt_ = std::chrono::steady_clock::now();
}

void Indexer::endFile() {
    std::lock_guard<std::mutex> lock(statusMutex_);
    currentInProgress_ = false;
}

namespace {

// Expected extracted-text size by type: the cost of indexing a file (time
// and RAM) follows its text, not its size on disk. XLSX is compressed XML
// that routinely inflates several-fold (60 MB on disk -> 378 MB of text in
// testing); a PDF is mostly images and fonts, so its text is a fraction.
std::uint64_t estimatedTextBytes(const FileRecord& record) {
    const std::string ext = toLowerAscii(record.extension);
    if (!ContentExtractor::isSupportedExtension(ext)) return 0;
    if (ext == ".xlsx") return record.size * 6;
    if (ext == ".docx") return record.size * 2;
    if (ext == ".pdf") return record.size / 4;
    return record.size;
}

} // namespace

bool Indexer::isHeavy(const FileRecord& record) const {
    const std::uint64_t threshold = indexerOptions_.heavyFileThreshold;
    return threshold != 0 && estimatedTextBytes(record) > threshold;
}

IndexerStatus Indexer::status() const {
    IndexerStatus s;
    s.phase = phase_.load();
    s.reconcile = reconcileMode_.load();
    s.pauseRequested = paused_.load();
    s.cancelRequested = cancelled_.load();
    s.cancelled = finishedCancelled_.load();
    const std::size_t active = activeWorkers_.load();
    s.paused = s.pauseRequested && active > 0 && parkedWorkers_.load() >= active;
    s.filesTotal = filesTotal_.load();
    s.totalIsEstimate = totalIsEstimate_.load();
    s.filesVisited = filesVisited_.load();
    s.filesWritten = filesWritten_.load();
    s.filesFailed = filesFailed_.load();
    s.heavyFound = heavyFound_.load();
    s.heavyDone = heavyDone_.load();
    s.heavyBytesTotal = heavyBytesTotal_.load();
    s.heavyBytesDone = heavyBytesDone_.load();
    s.heavyWorkTotal = heavyWorkTotal_.load();
    s.heavyWorkDone = heavyWorkDone_.load();
    s.processingHeavy = processingHeavy_.load();
    std::lock_guard<std::mutex> lock(statusMutex_);
    const std::uint64_t activeMs = heavyActiveMs_.load();
    if (s.processingHeavy && activeMs > 0 && s.heavyWorkDone > 0) {
        const double bytesPerSecond = static_cast<double>(s.heavyWorkDone) * 1000.0 / activeMs;
        const std::uint64_t remaining = s.heavyWorkTotal - std::min(s.heavyWorkDone, s.heavyWorkTotal);
        const std::uint64_t current = std::min(currentWork_.load(), remaining);
        double left = static_cast<double>(remaining) / bytesPerSecond;
        if (currentInProgress_) {
            const double onCurrent =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - currentStartedAt_).count();
            // The current file counts down as it runs, but never below
            // nothing: a file slower than forecast just holds its share at 0.
            left -= std::min(onCurrent, static_cast<double>(current) / bytesPerSecond);
        }
        s.heavySecondsLeft = std::max(0.0, left);
    }
    s.currentPath = currentPath_;
    s.currentSize = currentSize_;
    s.currentInProgress = currentInProgress_;
    s.currentStartedAt = currentStartedAt_;
    s.startedAt = startedAt_;
    s.finishedAt = finishedAt_;
    return s;
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
        // A reconcile pass is itself mostly a directory walk over unchanged
        // files — counting them first would double its cost, so the size of
        // the existing index stands in for the total.
        filesTotal_.store(existingByPath.size());
        totalIsEstimate_.store(true);
    } else {
        // A metadata-only walk up front so progress can read "N of M". Cheap
        // next to the indexing pass itself, which reads file contents.
        activeWorkers_.store(1);
        for (const auto& root : roots) {
            if (cancelled_.load(std::memory_order_relaxed)) break;
            if (!FileScanner::isAccessible(root)) continue;
            FileScanner::scan(
                root, scanOptions_,
                [this](const FileRecord& record) {
                    waitWhilePaused();
                    filesTotal_.fetch_add(1, std::memory_order_relaxed);
                    if (isHeavy(record)) {
                        ++heavyFound_;
                        heavyBytesTotal_ += record.size;
                    }
                },
                &cancelled_);
        }
        activeWorkers_.store(0);
        phase_.store(IndexPhase::Indexing);
    }

    storage_.beginBatch();

    std::atomic<std::size_t> nextRootIndex{0};
    std::atomic<bool> anyRootUnavailable{false};
    std::vector<FileRecord> deferred;
    std::mutex deferredMutex;

    const std::size_t threadCount = std::max<std::size_t>(
        1, std::min(roots.empty() ? std::size_t{1} : roots.size(),
                     indexerOptions_.threadCount == 0 ? defaultThreadCount() : indexerOptions_.threadCount));
    const std::uint64_t effectiveBatchSize =
        indexerOptions_.batchSize == 0 ? kDefaultBatchSize : indexerOptions_.batchSize;

    // Extracts and stores one file. Runs on a plain std::thread, not a Qt or
    // main thread — an exception escaping here (a malformed DOCX/PDF
    // tripping up the hand-rolled parsers, a SQLite error, anything) has no
    // thread to propagate to and calls std::terminate(), silently killing
    // the whole app. One bad file must not be able to do that: skip it and
    // keep indexing the rest.
    auto processFile = [&](const FileRecord& record) {
        try {
            std::string content;
            const std::string ext = toLowerAscii(record.extension);
            if (ContentExtractor::isSupportedExtension(ext)) {
                if (auto extracted = ContentExtractor::extract(pathFromUtf8(record.path), ext, extractionOptions_)) {
                    content = std::move(*extracted);
                }
            }
            storage_.upsertFile(record, content);
        } catch (const std::exception& e) {
            ++filesFailed_;
            ++filesVisited_;
            endFile();
            if (indexerOptions_.onFileError) indexerOptions_.onFileError(record.path, e.what());
            return;
        } catch (...) {
            ++filesFailed_;
            ++filesVisited_;
            endFile();
            if (indexerOptions_.onFileError) indexerOptions_.onFileError(record.path, "unknown error");
            return;
        }

        const std::uint64_t countSnapshot = ++filesWritten_;
        ++filesVisited_;
        endFile();
        if (countSnapshot % effectiveBatchSize == 0) {
            storage_.commitBatch();
            storage_.beginBatch();
        }
        if (onProgress) onProgress(IndexProgress{countSnapshot, record.path});
        if (indexerOptions_.ioDelayPerFile.count() > 0) {
            std::this_thread::sleep_for(indexerOptions_.ioDelayPerFile);
        }
    };

    auto worker = [&]() {
        if (indexerOptions_.onWorkerThreadStart) indexerOptions_.onWorkerThreadStart();
        ++activeWorkers_;

        while (!cancelled_.load(std::memory_order_relaxed)) {
            const std::size_t idx = nextRootIndex.fetch_add(1, std::memory_order_relaxed);
            if (idx >= roots.size()) break;

            // A temporarily unreachable source (ТЗ п.11.4, e.g. a
            // disconnected network drive) must not be treated as "every
            // indexed file was deleted" — skip it untouched instead.
            if (!FileScanner::isAccessible(roots[idx])) {
                anyRootUnavailable.store(true, std::memory_order_relaxed);
                if (indexerOptions_.onRootUnavailable) indexerOptions_.onRootUnavailable(roots[idx]);
                continue;
            }

            FileScanner::scan(
                roots[idx], scanOptions_,
                [&](const FileRecord& record) {
                    waitWhilePaused();
                    if (cancelled_.load(std::memory_order_relaxed)) return;
                    beginFile(record.path, record.size);

                    if (reconcileMode) {
                        {
                            std::lock_guard<std::mutex> lock(visitedPathsMutex);
                            visitedPaths.insert(record.path);
                        }
                        const auto it = existingByPath.find(record.path);
                        if (it != existingByPath.end() && it->second.size == record.size &&
                            it->second.modifiedTime == record.modifiedTime) {
                            ++filesVisited_;
                            endFile();
                            return;  // unchanged: nothing to do (ТЗ п.13.2)
                        }
                    }

                    if (isHeavy(record)) {
                        endFile();
                        std::lock_guard<std::mutex> lock(deferredMutex);
                        deferred.push_back(record);
                        return;
                    }
                    processFile(record);
                },
                &cancelled_);
        }
        --activeWorkers_;
    };

    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (std::size_t i = 0; i < threadCount; ++i) workers.emplace_back(worker);
    for (auto& t : workers) t.join();

    // Heavy files last, one at a time. The main pass's own list is the
    // authority now: the counting pass's figures were a forecast (and a
    // reconcile pass had none).
    if (!deferred.empty() && !cancelled_.load(std::memory_order_relaxed)) {
        std::uint64_t bytes = 0;
        std::uint64_t work = 0;
        for (const auto& record : deferred) {
            bytes += record.size;
            work += estimatedTextBytes(record);
        }
        heavyFound_.store(deferred.size());
        heavyBytesTotal_.store(bytes);
        heavyWorkTotal_.store(work);
        processingHeavy_.store(true);
        activeWorkers_.store(1);
        for (const auto& record : deferred) {
            waitWhilePaused();
            if (cancelled_.load(std::memory_order_relaxed)) break;
            const std::uint64_t work = estimatedTextBytes(record);
            currentWork_.store(work);
            beginFile(record.path, record.size);
            const auto started = std::chrono::steady_clock::now();
            processFile(record);
            heavyActiveMs_ += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                    .count());
            ++heavyDone_;
            heavyBytesDone_ += record.size;
            heavyWorkDone_ += work;
        }
        activeWorkers_.store(0);
        processingHeavy_.store(false);
    }

    phase_.store(IndexPhase::Finishing);
    if (reconcileMode && !cancelled_.load(std::memory_order_relaxed) &&
        !anyRootUnavailable.load(std::memory_order_relaxed)) {
        // Anything still indexed but never visited on this pass no longer
        // exists on disk (ТЗ п.13.2: "путь есть в индексе, но физически
        // отсутствует на диске -> запись удаляется"). Skipped entirely if any
        // root was unreachable this pass — we can't tell which existing
        // records belong to the unavailable root, so it's safer to remove
        // nothing than to wrongly wipe files that are simply offline.
        for (const auto& [path, stat] : existingByPath) {
            (void)stat;
            if (visitedPaths.find(path) == visitedPaths.end()) {
                storage_.removeFile(path);
            }
        }
    }

    storage_.commitBatch();

    const bool wasCancelled = cancelled_.load(std::memory_order_relaxed);
    finishedCancelled_.store(wasCancelled);
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        currentInProgress_ = false;
        finishedAt_ = std::chrono::steady_clock::now();
    }
    phase_.store(IndexPhase::Finished);
    running_.store(false, std::memory_order_relaxed);
    if (onComplete) {
        onComplete(wasCancelled);
    }
}

} // namespace datasearch::core
