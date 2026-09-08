#include "TestFramework.h"

#include "datasearch/core/IndexStorage.h"
#include "datasearch/core/SearchEngine.h"

#include <chrono>
#include <memory>
#include <sstream>
#include <vector>

using namespace datasearch::core;

namespace {

FileRecord makeRecord(int sourceIdx, int i) {
    FileRecord r;
    std::ostringstream pathStream;
    pathStream << "D:/source" << sourceIdx << "/folder" << (i % 500) << "/document_" << i << ".txt";
    r.path = pathStream.str();
    std::ostringstream nameStream;
    nameStream << "document_" << i << ".txt";
    r.name = nameStream.str();
    r.extension = ".txt";
    r.size = 1000 + static_cast<std::uint64_t>(i % 5000);
    r.modifiedTime = 1000000 + i;
    return r;
}

// A made-up word, present in exactly 1-in-1000 records, so an exact-match
// search against it exercises the FTS5 index over the full record set while
// still returning a small, predictable result count.
const char* kNeedleUtf8 = "\u0443\u043d\u0438\u043a\u0430\u043b\u044c\u043d\u044b\u0439\u0442\u0435\u0440\u043c\u0438\u043d";

std::string makeContent(int i) {
    std::ostringstream oss;
    oss << "\u0435\u0436\u0435\u043c\u0435\u0441\u044f\u0447\u043d\u044b\u0439 \u043e\u0442\u0447\u0451\u0442 "
           "\u043d\u043e\u043c\u0435\u0440 "
        << i << " \u043f\u043e \u043f\u0440\u043e\u0435\u043a\u0442\u0443 \u0430\u043b\u044c\u0444\u0430 "
                "\u0431\u0435\u0442\u0430 \u0433\u0430\u043c\u043c\u0430 \u0434\u0435\u043b\u044c\u0442\u0430. ";
    if (i % 1000 == 0) oss << kNeedleUtf8 << " ";
    oss << "\u043f\u0440\u0430\u043a\u0442\u0438\u043a\u0443\u043c \u0432\u0435\u0440\u0441\u0438\u044f "
           "\u0447\u0435\u0440\u043d\u043e\u0432\u0438\u043a \u0444\u0438\u043d\u0430\u043b \u0434\u043e\u043a\u0443"
           "\u043c\u0435\u043d\u0442 \u0444\u0430\u0439\u043b";
    return oss.str();
}

} // namespace

// Regression guard for ТЗ NFR-2 ("поиск по индексу — не более 200-300 мс...
// при одновременном поиске по нескольким выбранным дискам"). Uses a smaller
// (still substantial) record count than the ТЗ's 1M-5M target to stay fast
// in an ordinary test run — full-scale verification belongs on real target
// hardware with real data, not a unit test. This still exercises the actual
// FTS5 index, the Russian tokenizer, and SearchEngine's multi-source merge
// at a size where an unindexed/linear-scan approach would already be visibly
// slow, so it catches real regressions in the query path.
void runSearchPerformanceTests() {
    constexpr int kRecordsPerSource = 50000;
    constexpr int kSources = 3;
    constexpr int kCommitEvery = 1000;

    std::vector<std::unique_ptr<IndexStorage>> storages;
    for (int s = 0; s < kSources; ++s) {
        storages.push_back(std::make_unique<IndexStorage>(":memory:"));
    }

    const auto buildStart = std::chrono::steady_clock::now();
    for (int s = 0; s < kSources; ++s) {
        storages[static_cast<std::size_t>(s)]->beginBatch();
        for (int i = 0; i < kRecordsPerSource; ++i) {
            storages[static_cast<std::size_t>(s)]->upsertFile(makeRecord(s, i), makeContent(i));
            if ((i + 1) % kCommitEvery == 0) {
                storages[static_cast<std::size_t>(s)]->commitBatch();
                storages[static_cast<std::size_t>(s)]->beginBatch();
            }
        }
        storages[static_cast<std::size_t>(s)]->commitBatch();
    }
    const auto buildEnd = std::chrono::steady_clock::now();
    const auto buildMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(buildEnd - buildStart).count();

    std::vector<IndexStorage*> raw;
    raw.reserve(storages.size());
    for (auto& s : storages) raw.push_back(s.get());
    SearchEngine engine(raw);

    // Rare-term content search across all sources combined.
    SearchQuery needleQuery;
    needleQuery.namePattern = kNeedleUtf8;
    needleQuery.limit = 200;

    const auto searchStart = std::chrono::steady_clock::now();
    auto needleResults = engine.search(needleQuery);
    const auto searchEnd = std::chrono::steady_clock::now();
    const auto searchMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(searchEnd - searchStart).count();

    const auto expectedHits =
        static_cast<std::size_t>(kSources) * static_cast<std::size_t>(kRecordsPerSource / 1000);
    DS_CHECK_EQ(needleResults.size(), expectedHits);

    if (searchMs > 300) {
        throw std::runtime_error(
            "content search took " + std::to_string(searchMs) + " ms across " +
            std::to_string(static_cast<long long>(kSources) * kRecordsPerSource) +
            " records (index build took " + std::to_string(buildMs) + " ms) — exceeds ТЗ NFR-2's 300ms budget");
    }

    // A common bareword-prefix search (larger, more typical result set).
    SearchQuery prefixQuery;
    prefixQuery.namePattern = "document_1";
    prefixQuery.limit = 200;

    const auto search2Start = std::chrono::steady_clock::now();
    auto prefixResults = engine.search(prefixQuery);
    const auto search2End = std::chrono::steady_clock::now();
    const auto search2Ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(search2End - search2Start).count();

    DS_CHECK(!prefixResults.empty());
    if (search2Ms > 300) {
        throw std::runtime_error("prefix search took " + std::to_string(search2Ms) +
                                  " ms — exceeds ТЗ NFR-2's 300ms budget");
    }
}
