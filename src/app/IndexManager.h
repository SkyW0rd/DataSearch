#pragma once

#include "datasearch/core/IndexStorage.h"
#include "datasearch/core/Indexer.h"
#include "datasearch/core/SourceRegistry.h"
#include "datasearch/platform/IPlatformService.h"

#include <QObject>
#include <QString>
#include <QTimer>

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
    // silently skipped).
    std::vector<datasearch::core::FileRecord> search(const datasearch::core::SearchQuery& query,
                                                       const std::vector<std::string>& roots);

    // Masks applied to all subsequent full scans, reconciliation passes and
    // live-watch updates (ТЗ FR-8). Does not retroactively re-scan already
    // matching files.
    void setExcludeMasks(std::vector<std::string> masks);

    // Roots currently known (reopened from a previous session or indexed
    // this session) — lets the UI show them as available/checked even before
    // the user re-selects them from the volume list.
    std::vector<std::string> knownRoots() const;

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
    datasearch::core::IndexerOptions makeIndexerOptions() const;

    // Opens/creates storage+indexer for `root` if not already present.
    datasearch::core::IndexStorage& ensureStorage(const std::string& root);
    void startWatch(const std::string& root);
    void watcherThreadMain();
    void applyChange(datasearch::core::IndexStorage& storage, const std::string& root,
                      const std::string& pathUtf8, int kindInt);

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
};
