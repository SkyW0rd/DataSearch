#include "TestFramework.h"

#include "datasearch/core/FileScanner.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <random>

using datasearch::core::FileRecord;
using datasearch::core::FileScanner;
using datasearch::core::ScanOptions;

namespace {

std::filesystem::path makeScratchDir() {
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() /
               ("datasearch_scanner_test_" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    return dir;
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

} // namespace

void runFileScannerTests() {
    const auto root = makeScratchDir();
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{root};

    writeFile(root / "alpha.txt", "hello");
    writeFile(root / "beta.log", "12345");
    writeFile(root / "sub" / "gamma.txt", "xyz");
    writeFile(root / "skip.tmp", "should be excluded by mask");
    writeFile(root / "node_modules" / "ignored.js", "should be excluded, dir pruned");

    ScanOptions options;
    options.excludeMasks = {"*.tmp", "node_modules"};

    std::map<std::string, FileRecord> byName;
    FileScanner::scan(root, options, [&](const FileRecord& record) {
        byName[record.name] = record;
    });

    DS_CHECK_EQ(byName.size(), std::size_t{3});
    DS_CHECK(byName.count("alpha.txt") == 1);
    DS_CHECK(byName.count("beta.log") == 1);
    DS_CHECK(byName.count("gamma.txt") == 1);
    DS_CHECK(byName.count("skip.tmp") == 0);
    DS_CHECK(byName.count("ignored.js") == 0);

    const auto& alpha = byName.at("alpha.txt");
    DS_CHECK_EQ(alpha.size, std::uint64_t{5});
    DS_CHECK_EQ(alpha.extension, std::string(".txt"));
    DS_CHECK(alpha.modifiedTime > 0);

    const auto& gamma = byName.at("gamma.txt");
    DS_CHECK(gamma.path.find("sub") != std::string::npos);

    // Cancellation: an already-cancelled token must stop the scan before it visits anything.
    std::atomic<bool> cancelled{true};
    std::size_t visitedAfterCancel = 0;
    FileScanner::scan(root, {}, [&](const FileRecord&) { ++visitedAfterCancel; }, &cancelled);
    DS_CHECK_EQ(visitedAfterCancel, std::size_t{0});
}
