#include "IndexManager.h"

#include "datasearch/core/ContentExtractor.h"
#include "datasearch/core/FileScanner.h"
#include "datasearch/core/SearchEngine.h"
#include "datasearch/core/Utf8.h"

#include <QDir>
#include <QStandardPaths>

#include <algorithm>
#include <cctype>
#include <chrono>

using datasearch::core::ExtractionOptions;
using datasearch::core::FileRecord;
using datasearch::core::IndexProgress;
using datasearch::core::Indexer;
using datasearch::core::IndexerOptions;
using datasearch::core::IndexStorage;
using datasearch::core::ScanOptions;
using datasearch::core::SearchEngine;
using datasearch::core::SearchQuery;
using datasearch::core::SourceEntry;
using datasearch::platform::FileSystemChange;
using datasearch::platform::IPlatformService;

namespace {

std::string sanitizeForFilename(const std::string& root) {
    std::string out = root;
    for (char& c : out) {
        if (c == ':' || c == '\\' || c == '/' || c == ' ') c = '_';
    }
    return out;
}

std::int64_t nowEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

IndexManager::IndexManager(IPlatformService* platform, QObject* parent)
    : QObject(parent), platform_(platform) {
    const QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(baseDir).mkpath(".");
    registry_ = std::make_unique<datasearch::core::SourceRegistry>(
        datasearch::core::pathFromUtf8(QDir(baseDir).filePath("registry.sqlite3").toStdString()));

    debounceTimer_.setSingleShot(true);
    debounceTimer_.setInterval(500);
    connect(&debounceTimer_, &QTimer::timeout, this, &IndexManager::onDebounceTimeout);
    connect(this, &IndexManager::rawFileSystemChange, this, &IndexManager::onRawFileSystemChange);

    // Whenever a full index or a reconciliation pass completes successfully,
    // remember the source for next startup and make sure it's being watched
    // live (ТЗ FR-23/25). Connected via queued AutoConnection since
    // `finished` can be emitted from a background Indexer thread.
    connect(this, &IndexManager::finished, this, [this](QString rootLabel, bool cancelled) {
        if (cancelled) return;
        const std::string root = rootLabel.toStdString();
        {
            std::lock_guard<std::mutex> lock(mapsMutex_);
            if (storages_.find(root) == storages_.end()) return;
        }
        SourceEntry entry;
        entry.root = root;
        entry.dbPath = datasearch::core::pathToUtf8(dbPathFor(root));
        entry.lastFullScan = nowEpochSeconds();
        try {
            registry_->upsert(entry);
        } catch (const std::exception&) {
            // Best-effort: the source still works this session even if we
            // can't remember it for next time.
        }
        startWatch(root);
    });

    watcherThread_ = std::thread(&IndexManager::watcherThreadMain, this);
}

IndexManager::~IndexManager() {
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        stopping_ = true;
    }
    pendingCv_.notify_all();
    if (watcherThread_.joinable()) watcherThread_.join();
}

std::filesystem::path IndexManager::dbPathFor(const std::string& root) {
    const QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(baseDir);
    dir.mkpath("index");
    const QString fileName = QString::fromStdString(sanitizeForFilename(root)) + ".sqlite3";
    return datasearch::core::pathFromUtf8(dir.filePath("index/" + fileName).toStdString());
}

ScanOptions IndexManager::currentScanOptions() const {
    ScanOptions options;
    std::lock_guard<std::mutex> lock(mapsMutex_);
    options.excludeMasks = excludeMasks_;
    return options;
}

IndexerOptions IndexManager::makeIndexerOptions() const {
    IndexerOptions options;
    IPlatformService* platform = platform_;
    options.onWorkerThreadStart = [platform]() {
        if (platform == nullptr) return;
        try {
            platform->lowerCurrentThreadPriority();
        } catch (...) {
            // Best-effort (ТЗ п.12.3): indexing still works at normal priority.
        }
    };
    options.onRootUnavailable = [this](const std::filesystem::path& root) {
        emit sourceUnavailable(QString::fromStdString(datasearch::core::pathToUtf8(root)));
    };
    return options;
}

void IndexManager::setExcludeMasks(std::vector<std::string> masks) {
    std::lock_guard<std::mutex> lock(mapsMutex_);
    excludeMasks_ = std::move(masks);
}

IndexStorage& IndexManager::ensureStorage(const std::string& root) {
    std::lock_guard<std::mutex> lock(mapsMutex_);
    auto it = storages_.find(root);
    if (it == storages_.end()) {
        it = storages_.emplace(root, std::make_unique<IndexStorage>(dbPathFor(root))).first;
    }
    return *it->second;
}

std::vector<std::string> IndexManager::knownRoots() const {
    std::lock_guard<std::mutex> lock(mapsMutex_);
    std::vector<std::string> roots;
    roots.reserve(storages_.size());
    for (const auto& [root, storage] : storages_) roots.push_back(root);
    return roots;
}

void IndexManager::pauseAllIndexing() {
    std::lock_guard<std::mutex> lock(mapsMutex_);
    for (auto& [root, indexer] : indexers_) {
        (void)root;
        indexer->pause();
    }
}

void IndexManager::resumeAllIndexing() {
    std::lock_guard<std::mutex> lock(mapsMutex_);
    for (auto& [root, indexer] : indexers_) {
        (void)root;
        indexer->resume();
    }
}

bool IndexManager::isAnyIndexingPaused() const {
    std::lock_guard<std::mutex> lock(mapsMutex_);
    for (const auto& [root, indexer] : indexers_) {
        (void)root;
        if (indexer->isRunning() && indexer->isPaused()) return true;
    }
    return false;
}

void IndexManager::loadKnownSources() {
    for (const auto& entry : registry_->all()) {
        try {
            IndexStorage& storage = ensureStorage(entry.root);

            auto indexer =
                std::make_unique<Indexer>(storage, currentScanOptions(), ExtractionOptions{}, makeIndexerOptions());
            Indexer* indexerPtr = indexer.get();
            {
                std::lock_guard<std::mutex> lock(mapsMutex_);
                indexers_[entry.root] = std::move(indexer);
            }

            const QString rootLabel = QString::fromStdString(entry.root);
            indexerPtr->startReconcile(
                {datasearch::core::pathFromUtf8(entry.root)},
                [this, rootLabel](const IndexProgress& p) {
                    emit progress(p.filesIndexed, QString::fromStdString(p.currentPath), rootLabel);
                },
                [this, rootLabel](bool cancelled) { emit finished(rootLabel, cancelled); });
        } catch (const std::exception& e) {
            emit watcherActivity(QString::fromStdString(entry.root),
                                  tr("Не удалось открыть индекс: %1").arg(e.what()));
        }
    }
}

void IndexManager::indexRoots(const std::vector<std::string>& roots) {
    for (const auto& root : roots) {
        IndexStorage& storage = ensureStorage(root);

        auto indexer =
            std::make_unique<Indexer>(storage, currentScanOptions(), ExtractionOptions{}, makeIndexerOptions());
        Indexer* indexerPtr = indexer.get();
        {
            std::lock_guard<std::mutex> lock(mapsMutex_);
            indexers_[root] = std::move(indexer);
        }

        const QString rootLabel = QString::fromStdString(root);
        indexerPtr->start(
            {datasearch::core::pathFromUtf8(root)},
            [this, rootLabel](const IndexProgress& p) {
                emit progress(p.filesIndexed, QString::fromStdString(p.currentPath), rootLabel);
            },
            [this, rootLabel](bool cancelled) { emit finished(rootLabel, cancelled); });
    }
}

std::vector<FileRecord> IndexManager::search(const SearchQuery& query, const std::vector<std::string>& roots) {
    std::vector<IndexStorage*> sources;
    {
        std::lock_guard<std::mutex> lock(mapsMutex_);
        for (const auto& root : roots) {
            auto it = storages_.find(root);
            if (it != storages_.end()) sources.push_back(it->second.get());
        }
    }
    if (sources.empty()) return {};

    SearchEngine engine(sources);
    return engine.search(query);
}

void IndexManager::startWatch(const std::string& root) {
    if (platform_ == nullptr) return;

    {
        std::lock_guard<std::mutex> lock(mapsMutex_);
        if (watches_.find(root) != watches_.end()) return;  // already watching
    }

    try {
        auto watch = platform_->watchDirectory(
            datasearch::core::pathFromUtf8(root), [this, root](const FileSystemChange& change) {
                emit rawFileSystemChange(QString::fromStdString(root),
                                          QString::fromStdString(datasearch::core::pathToUtf8(change.path)),
                                          static_cast<int>(change.kind));
            });
        std::lock_guard<std::mutex> lock(mapsMutex_);
        watches_[root] = std::move(watch);
    } catch (const std::exception& e) {
        emit watcherActivity(QString::fromStdString(root),
                              tr("Не удалось включить слежение за изменениями: %1").arg(e.what()));
    }
}

void IndexManager::onRawFileSystemChange(QString root, QString path, int kind) {
    stagedByRoot_[root.toStdString()][path.toStdString()] = kind;
    debounceTimer_.start();  // (re)start the coalescing window
}

void IndexManager::onDebounceTimeout() {
    if (stagedByRoot_.empty()) return;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        for (auto& [root, changes] : stagedByRoot_) {
            auto& target = pendingByRoot_[root];
            for (auto& [path, kind] : changes) target[path] = kind;
        }
    }
    stagedByRoot_.clear();
    pendingCv_.notify_one();
}

void IndexManager::watcherThreadMain() {
    while (true) {
        std::map<std::string, std::map<std::string, int>> batch;
        {
            std::unique_lock<std::mutex> lock(pendingMutex_);
            pendingCv_.wait(lock, [this] { return stopping_ || !pendingByRoot_.empty(); });
            if (stopping_ && pendingByRoot_.empty()) return;
            batch.swap(pendingByRoot_);
        }

        for (auto& [root, changes] : batch) {
            IndexStorage* storage = nullptr;
            {
                std::lock_guard<std::mutex> lock(mapsMutex_);
                auto it = storages_.find(root);
                if (it != storages_.end()) storage = it->second.get();
            }
            if (storage == nullptr) continue;

            for (auto& [path, kind] : changes) {
                applyChange(*storage, root, path, kind);
            }
            emit watcherActivity(QString::fromStdString(root),
                                  tr("Обновлено файлов: %1").arg(changes.size()));
        }
    }
}

void IndexManager::applyChange(IndexStorage& storage, const std::string& root, const std::string& pathUtf8,
                                int kindInt) {
    (void)root;
    using Kind = FileSystemChange::Kind;
    const auto kind = static_cast<Kind>(kindInt);

    if (kind == Kind::Removed) {
        storage.removeFile(pathUtf8);
        return;
    }

    const auto fsPath = datasearch::core::pathFromUtf8(pathUtf8);
    const auto stat = datasearch::core::FileScanner::statFile(fsPath, currentScanOptions());
    if (!stat) {
        // Gone again, is a directory, or matches an exclude mask (ТЗ FR-8) —
        // either way it shouldn't be in the index.
        storage.removeFile(pathUtf8);
        return;
    }

    FileRecord record = *stat;
    std::string content;
    std::string ext = record.extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (datasearch::core::ContentExtractor::isSupportedExtension(ext)) {
        if (auto extracted = datasearch::core::ContentExtractor::extract(fsPath, ext)) {
            content = std::move(*extracted);
        }
    }
    storage.upsertFile(record, content);
}
