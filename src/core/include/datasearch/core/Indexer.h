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

    void waitWhilePaused();
    void runInternal(std::vector<std::filesystem::path> roots,
                      ProgressCallback onProgress,
                      CompletionCallback onComplete,
                      bool reconcileMode);
};

} // namespace datasearch::core
