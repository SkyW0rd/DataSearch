#pragma once

#include "datasearch/core/IndexStorage.h"
#include "datasearch/core/Indexer.h"
#include "datasearch/core/SourceRegistry.h"
#include "datasearch/platform/IPlatformService.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>

#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Owns one IndexStorage (+ Indexer, + live filesystem watch) per indexed
// source, per ТЗ п.11.2: "индекс каждого диска хранится отдельно". Lives on
// the GUI thread; Indexer and the platform watcher each run on their own
// background thread(s) and report back via queued signals.
//
// Startup/live-update behaviour (ТЗ п.13, FR-23/24/25):
//  - loadKnownSources() reopens every source from a previous session
//    instantly (just opening its SQLite file), then kicks off a background
//    metadata-reconciliation pass per source (п.13.2) rather than a full
//    reindex.
//  - Once a source is open (freshly indexed or reopened), a live watch is
//    started on it; filesystem-change events are coalesced (debounced) and
//    applied incrementally on a single dedicated background thread.
class IndexManager : public QObject {
    Q_OBJECT
public:
    // `platform` is non-owning and may be null (e.g. if platform-service init
    // failed) — thread-priority lowering and live-watching are then simply
    // skipped, search/manual indexing still work.
    explicit IndexManager(datasearch::platform::IPlatformService* platform, QObject* parent = nullptr);
    ~IndexManager() override;

    // Reopens every source registered in a previous session (see class
    // comment). Call once, after construction.
    void loadKnownSources();

    // Opens (or creates) the index for each root not already indexed and starts
    // a background full scan for it. On success, registers the source for
    // next-startup reopening and starts watching it live.
    void indexRoots(const std::vector<std::string>& roots);

    // Searches across the indexes for the given roots (only those that have
    // already been opened via indexRoots/loadKnownSources — others are
    // silently skipped). Runs on the calling thread — blocks it for however
    // long the query takes (ТЗ NFR-2 targets ≤300мс, but an unbounded query
    // like a single-character prefix against a huge index can take far
    // longer). Prefer searchAsync() from the GUI thread; this synchronous
    // form exists for tests and for searchAsync's own background thread.
    std::vector<datasearch::core::FileRecord> search(const datasearch::core::SearchQuery& query,
                                                       const std::vector<std::string>& roots);

    // Runs the search on a dedicated background thread so the GUI thread is
    // never blocked (ТЗ NFR-7: "UI отзывчив при любом объёме индекса"),
    // regardless of how long a particular query takes. `onDone` is delivered
    // on `context`'s thread via a queued call, and is automatically skipped
    // (not a dangling-pointer risk) if `context` is destroyed before the
    // search finishes — pass the requesting widget/object as `context`.
    // If a newer searchAsync() call arrives before an older one has started
    // running, the older one is dropped (never executed) — only the latest
    // query's results are ever delivered, exactly what's needed for
    // search-as-you-type.
    void searchAsync(datasearch::core::SearchQuery query, std::vector<std::string> roots, QObject* context,
                      std::function<void(std::vector<datasearch::core::FileRecord>)> onDone);

    // Masks applied to all subsequent full scans, reconciliation passes and
    // live-watch updates (ТЗ FR-8). Does not retroactively re-scan already
    // matching files.
    void setExcludeMasks(std::vector<std::string> masks);

    // Roots currently known (reopened from a previous session or indexed
    // this session) — lets the UI show them as available/checked even before
    // the user re-selects them from the volume list.
    std::vector<std::string> knownRoots() const;

    // Pauses/resumes every currently active background scan or
    // reconciliation (ТЗ п.12.3 UI: "поставить индексацию на паузу", e.g. so
    // the user can reclaim CPU/disk for a game or render). Files already in
    // flight finish normally; nothing already indexed is lost, and a paused
    // scan resumes exactly where it left off. Newly started scans are not
    // paused by this past state — it only affects what's running right now.
    void pauseAllIndexing();
    void resumeAllIndexing();
    bool isAnyIndexingPaused() const;

signals:
    void progress(quint64 filesIndexed, QString currentPath, QString rootLabel);
    void finished(QString rootLabel, bool cancelled);
    void watcherActivity(QString rootLabel, QString description);
    // A source was temporarily unreachable during a scan/reconcile (ТЗ
    // п.11.4, e.g. a disconnected network drive) — its existing index was
    // left untouched and still serves search results.
    void sourceUnavailable(QString rootLabel);

signals:
    // Internal: marshals a watcher callback (fires on the watch's own
    // background thread) onto the GUI thread via a queued signal/slot.
    void rawFileSystemChange(QString root, QString path, int kind);

private slots:
    void onRawFileSystemChange(QString root, QString path, int kind);
    void onDebounceTimeout();

private:
    static std::filesystem::path dbPathFor(const std::string& root);

    datasearch::core::ScanOptions currentScanOptions() const;
    datasearch::core::IndexerOptions makeIndexerOptions();

    // Opens/creates storage+indexer for `root` if not already present.
    datasearch::core::IndexStorage& ensureStorage(const std::string& root);
    void startWatch(const std::string& root);
    void watcherThreadMain();
    void applyChange(datasearch::core::IndexStorage& storage, const std::string& root,
                      const std::string& pathUtf8, int kindInt);
    void searchThreadMain();

    datasearch::platform::IPlatformService* platform_ = nullptr;
    std::unique_ptr<datasearch::core::SourceRegistry> registry_;

    // Guards excludeMasks_ (read by the watcher thread, written by the GUI
    // thread from Settings) and the three maps below (structurally modified
    // from the GUI thread, looked up from the watcher thread).
    mutable std::mutex mapsMutex_;
    std::vector<std::string> excludeMasks_;
    std::map<std::string, std::unique_ptr<datasearch::core::IndexStorage>> storages_;
    std::map<std::string, std::unique_ptr<datasearch::core::Indexer>> indexers_;
    std::map<std::string, std::unique_ptr<datasearch::platform::IDirectoryWatch>> watches_;

    // GUI-thread-only staging area, drained into pendingByRoot_ on debounce timeout.
    std::map<std::string, std::map<std::string, int>> stagedByRoot_;
    QTimer debounceTimer_;

    // Handoff to the dedicated watcher-processing thread.
    std::mutex pendingMutex_;
    std::condition_variable pendingCv_;
    std::map<std::string, std::map<std::string, int>> pendingByRoot_;
    bool stopping_ = false;
    std::thread watcherThread_;

    // Dedicated search worker: always processes only the *latest* pending
    // request (see searchAsync doc comment above).
    struct PendingSearch {
        datasearch::core::SearchQuery query;
        std::vector<std::string> roots;
        QObject* context = nullptr;
        std::function<void(std::vector<datasearch::core::FileRecord>)> onDone;
    };
    std::mutex searchMutex_;
    std::condition_variable searchCv_;
    std::optional<PendingSearch> pendingSearch_;
    bool searchStopping_ = false;
    std::thread searchThread_;
};
