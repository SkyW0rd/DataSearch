#include "IndexManager.h"

#include "datasearch/core/SearchEngine.h"
#include "datasearch/core/Utf8.h"

#include <QDir>
#include <QStandardPaths>

using datasearch::core::FileRecord;
using datasearch::core::IndexProgress;
using datasearch::core::Indexer;
using datasearch::core::IndexStorage;
using datasearch::core::ScanOptions;
using datasearch::core::SearchEngine;
using datasearch::core::SearchQuery;

namespace {

std::string sanitizeForFilename(const std::string& root) {
    std::string out = root;
    for (char& c : out) {
        if (c == ':' || c == '\\' || c == '/' || c == ' ') c = '_';
    }
    return out;
}

} // namespace

IndexManager::IndexManager(QObject* parent) : QObject(parent) {}

std::filesystem::path IndexManager::dbPathFor(const std::string& root) {
    const QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(baseDir);
    dir.mkpath("index");
    const QString fileName = QString::fromStdString(sanitizeForFilename(root)) + ".sqlite3";
    return datasearch::core::pathFromUtf8(dir.filePath("index/" + fileName).toStdString());
}

void IndexManager::indexRoots(const std::vector<std::string>& roots) {
    for (const auto& root : roots) {
        auto storageIt = storages_.find(root);
        if (storageIt == storages_.end()) {
            storageIt = storages_.emplace(root, std::make_unique<IndexStorage>(dbPathFor(root))).first;
        }
        IndexStorage& storage = *storageIt->second;

        auto indexer = std::make_unique<Indexer>(storage, ScanOptions{});
        Indexer* indexerPtr = indexer.get();
        indexers_[root] = std::move(indexer);

        const QString rootLabel = QString::fromStdString(root);
        indexerPtr->start(
            {datasearch::core::pathFromUtf8(root)},
            [this, rootLabel](const IndexProgress& p) {
                emit progress(p.filesIndexed, QString::fromStdString(p.currentPath), rootLabel);
            },
            [this, rootLabel](bool cancelled) { emit finished(rootLabel, cancelled); });
    }
}

std::vector<FileRecord> IndexManager::search(const SearchQuery& query,
                                              const std::vector<std::string>& roots) {
    std::vector<IndexStorage*> sources;
    for (const auto& root : roots) {
        auto it = storages_.find(root);
        if (it != storages_.end()) sources.push_back(it->second.get());
    }
    if (sources.empty()) return {};

    SearchEngine engine(sources);
    return engine.search(query);
}
