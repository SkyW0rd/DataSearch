#pragma once

#include "datasearch/core/IndexStorage.h"
#include "datasearch/core/Indexer.h"

#include <QObject>
#include <QString>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Owns one IndexStorage (+ Indexer) per indexed source, per ТЗ п.11.2: "индекс
// каждого диска хранится отдельно". Lives on the GUI thread; Indexer runs each
// scan on its own std::thread and reports back via queued signals.
class IndexManager : public QObject {
    Q_OBJECT
public:
    explicit IndexManager(QObject* parent = nullptr);

    // Opens (or creates) the index for each root not already indexed and starts
    // a background scan for it.
    void indexRoots(const std::vector<std::string>& roots);

    // Searches across the indexes for the given roots (only those that have
    // already been opened via indexRoots — others are silently skipped).
    std::vector<datasearch::core::FileRecord> search(const datasearch::core::SearchQuery& query,
                                                       const std::vector<std::string>& roots);

signals:
    void progress(quint64 filesIndexed, QString currentPath, QString rootLabel);
    void finished(QString rootLabel, bool cancelled);

private:
    static std::filesystem::path dbPathFor(const std::string& root);

    std::map<std::string, std::unique_ptr<datasearch::core::IndexStorage>> storages_;
    std::map<std::string, std::unique_ptr<datasearch::core::Indexer>> indexers_;
};
