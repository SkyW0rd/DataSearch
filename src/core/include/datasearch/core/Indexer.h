#pragma once

#include "datasearch/core/ContentExtractor.h"
#include "datasearch/core/FileScanner.h"
#include "datasearch/core/IndexStorage.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
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

enum class IndexPhase {
    Idle,       // never started
    Counting,   // walking the roots to learn how many files there are
    Indexing,   // processing files
    Finishing,  // final commit; when reconciling, also dropping vanished files
    Finished,
};

// Point-in-time view of a run for a progress display (see Indexer::status()).
struct IndexerStatus {
    IndexPhase phase = IndexPhase::Idle;
    bool reconcile = false;
    bool pauseRequested = false;
    // Pause requested AND every worker has actually stopped — until then the
    // file in flight is still being finished.
    bool paused = false;
    bool cancelRequested = false;
    bool cancelled = false;  // the run Finished because of cancel()

    // While Counting: files found so far. Afterwards: the total. When
    // reconciling there is no counting pass and this is the number of files
    // the index held before the pass — an estimate (`totalIsEstimate`).
    std::uint64_t filesTotal = 0;
    bool totalIsEstimate = false;
    std::uint64_t filesVisited = 0;  // written + unchanged + failed
    std::uint64_t filesWritten = 0;
    std::uint64_t filesFailed = 0;

    // Heavy files (IndexerOptions::heavyFileThreshold) are set aside during
    // the main pass and processed one at a time at the end.
    // `processingHeavy` is true during that final stretch.
    std::uint64_t heavyFound = 0;
    std::uint64_t heavyDone = 0;
    std::uint64_t heavyBytesTotal = 0;
    std::uint64_t heavyBytesDone = 0;
    // The same in expected extracted text, which tracks time far better
    // than size on disk (an XLSX costs ~6x a text file of equal size).
    std::uint64_t heavyWorkTotal = 0;
    std::uint64_t heavyWorkDone = 0;
    bool processingHeavy = false;
    // Seconds until the heavy files are done, or -1 while there's no basis
    // for a forecast yet (before the first one finishes). Paced only by
    // heavy files already processed, excluding pauses, and counting down
    // through the file in flight rather than stalling while it runs.
    double heavySecondsLeft = -1;

    // The file most recently started. `currentInProgress` is false once it's
    // done and the scan is between files (e.g. walking empty directories).
    std::string currentPath;
    std::uint64_t currentSize = 0;
    bool currentInProgress = false;
    std::chrono::steady_clock::time_point currentStartedAt;

    std::chrono::steady_clock::time_point startedAt;
    std::chrono::steady_clock::time_point finishedAt;
};

struct IndexerOptions {
    // Background-indexing parallelism cap (ТЗ п.12.3: "не более
    // количество_ядер/2, не менее 1 и не более 4"). 0 = pick that default.
    std::size_t threadCount = 0;

    // Records written per SQLite commit. Smaller values commit (and release
    // the batch's in-memory statement/WAL state) more often, trading some
    // throughput for a lower peak memory footprint — the "сброс буферов на
    // диск" behaviour ТЗ п.12.3 asks for under a constrained RAM budget.
    // 0 = use the built-in default (500).
    std::uint64_t batchSize = 0;

    // Pause briefly after each file (ТЗ п.12.3: "trottling между файлами...
    // особенно на HDD"), so background indexing doesn't saturate disk I/O
    // the user's foreground work also needs. 0 (default) = no throttling.
    std::chrono::milliseconds ioDelayPerFile{0};

    // Called once at the start of each indexing worker thread, so the
    // platform layer can lower its OS thread priority (Windows:
    // THREAD_PRIORITY_BELOW_NORMAL, ТЗ п.12.3) without core depending on
    // platform directly.
    std::function<void()> onWorkerThreadStart;

    // Called (on a worker thread) for a root that FileScanner::isAccessible()
    // finds unreachable (e.g. a disconnected network drive, ТЗ п.11.4) before
    // any scanning is attempted for it. That root is then skipped entirely —
    // in reconcile mode in particular, its previously-indexed files are left
    // untouched rather than being treated as deleted.
    std::function<void(const std::filesystem::path&)> onRootUnavailable;

    // Files whose extracted text is expected to exceed this many bytes are
    // "heavy": skipped in the main pass and indexed one at a time after it.
    // Everything else becomes searchable first, progress keeps moving instead
    // of stalling on one document, and only one memory-hungry file is ever in
    // flight. The estimate is by type and size (see Indexer.cpp). 0 = never
    // defer.
    std::uint64_t heavyFileThreshold = 20ull * 1024 * 1024;

    // Called (on a worker thread) when extracting or storing one file throws
    // — a malformed document, a filesystem-level error mid-read, etc. That
    // file is skipped (left as-is in the index if it was already there); the
    // scan continues with the rest. Optional — indexing works without it.
    std::function<void(const std::string& path, const std::string& what)> onFileError;
};

// Runs scans of one or more roots on a background thread pool and writes the
// results (metadata + extracted content, ТЗ FR-3) into `storage`, batching
// writes into transactions for throughput (ТЗ FR-4: индексация не блокирует
// UI). Several roots are scanned in parallel across up to
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

    // Full (re)index: every visited file is (re-)extracted and written,
    // regardless of what's already in `storage`.
    void start(std::vector<std::filesystem::path> roots,
               ProgressCallback onProgress = nullptr,
               CompletionCallback onComplete = nullptr);

    // Metadata-only reconciliation pass (ТЗ п.13.2 — "сверка по возвращении"):
    // walks `roots` comparing each file's size/modified-time against what's
    // already in `storage`, WITHOUT re-reading unchanged files. A file is
    // (re-)extracted only if it's new or its size/mtime differ from the
    // stored record; a stored path no longer present on disk is removed.
    // Much cheaper than start() for the common case where little changed
    // while the source wasn't being watched.
    void startReconcile(std::vector<std::filesystem::path> roots,
                         ProgressCallback onProgress = nullptr,
                         CompletionCallback onComplete = nullptr);

    // Requests the running scan to stop at the next visited entry. Does not block.
    void cancel();

    // Temporarily stops processing new files (ТЗ п.12.3 UI: "поставить
    // индексацию на паузу", e.g. so the user can reclaim CPU/disk for a game
    // or render). The file currently being processed finishes normally;
    // scanning resumes exactly where it left off — unlike cancel(), nothing
    // is lost. Does not block.
    void pause();
    void resume();
    bool isPaused() const;

    // Blocks until the background thread finishes (a no-op if none is running).
    void join();

    bool isRunning() const;

    // Cheap and safe to call from any thread at any time, e.g. from a UI
    // timer; the last run's final state stays readable after it Finished.
    IndexerStatus status() const;

private:
    static constexpr std::uint64_t kDefaultBatchSize = 500;

    IndexStorage& storage_;
    ScanOptions scanOptions_;
    ExtractionOptions extractionOptions_;
    IndexerOptions indexerOptions_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> running_{false};
    std::mutex pauseMutex_;
    std::condition_variable pauseCv_;
    std::thread worker_;

    std::atomic<IndexPhase> phase_{IndexPhase::Idle};
    std::atomic<bool> reconcileMode_{false};
    std::atomic<bool> finishedCancelled_{false};
    std::atomic<bool> totalIsEstimate_{false};
    std::atomic<std::uint64_t> filesTotal_{0};
    std::atomic<std::uint64_t> filesVisited_{0};
    std::atomic<std::uint64_t> filesWritten_{0};
    std::atomic<std::uint64_t> filesFailed_{0};
    std::atomic<std::size_t> activeWorkers_{0};
    std::atomic<std::size_t> parkedWorkers_{0};
    std::atomic<std::uint64_t> heavyFound_{0};
    std::atomic<std::uint64_t> heavyDone_{0};
    std::atomic<std::uint64_t> heavyBytesTotal_{0};
    std::atomic<std::uint64_t> heavyBytesDone_{0};
    std::atomic<std::uint64_t> heavyWorkTotal_{0};
    std::atomic<std::uint64_t> heavyWorkDone_{0};
    std::atomic<std::uint64_t> heavyActiveMs_{0};  // time spent processing finished heavy files
    std::atomic<std::uint64_t> currentWork_{0};     // expected text of the heavy file in flight
    std::atomic<bool> processingHeavy_{false};

    mutable std::mutex statusMutex_;  // guards the fields below
    std::string currentPath_;
    std::uint64_t currentSize_ = 0;
    bool currentInProgress_ = false;
    std::chrono::steady_clock::time_point currentStartedAt_;
    std::chrono::steady_clock::time_point startedAt_;
    std::chrono::steady_clock::time_point finishedAt_;

    void launch(std::vector<std::filesystem::path> roots, ProgressCallback onProgress,
                CompletionCallback onComplete, bool reconcileMode);
    void waitWhilePaused();
    void beginFile(const std::string& path, std::uint64_t size);
    void endFile();
    bool isHeavy(const FileRecord& record) const;
    void runInternal(std::vector<std::filesystem::path> roots,
                      ProgressCallback onProgress,
                      CompletionCallback onComplete,
                      bool reconcileMode);
};

} // namespace datasearch::core
