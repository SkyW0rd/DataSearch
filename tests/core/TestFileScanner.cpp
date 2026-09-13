#include "TestFramework.h"

#include "datasearch/core/FileScanner.h"

#include <chrono>
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

    // statFile: single-file metadata, used to apply one live-watcher event
    // without rescanning the whole tree (ТЗ FR-6/п.13.2).
    auto stat = FileScanner::statFile(root / "alpha.txt", options);
    DS_CHECK(stat.has_value());
    DS_CHECK_EQ(stat->name, std::string("alpha.txt"));
    DS_CHECK_EQ(stat->size, std::uint64_t{5});

    // A modification time on a whole second (common on exFAT/FAT disks)
    // converts to the same value every time and in both the walk and
    // statFile — a second off on some runs made the startup check read such
    // files again as "modified". Half a second later is the same second.
    {
        using namespace std::chrono;
        const auto whole = time_point_cast<seconds>(std::filesystem::file_time_type::clock::now() - hours(24));
        std::filesystem::last_write_time(root / "alpha.txt", std::filesystem::file_time_type(whole));
        const std::int64_t first = FileScanner::statFile(root / "alpha.txt", options)->modifiedTime;
        const std::int64_t now = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
        DS_CHECK(first > now - 24 * 3600 - 5 && first < now - 24 * 3600 + 5);
        for (int i = 0; i < 20000; ++i) {
            if (FileScanner::statFile(root / "alpha.txt", options)->modifiedTime != first) {
                DS_CHECK(false && "whole-second mtime converted inconsistently");
            }
        }
        std::int64_t walked = 0;
        FileScanner::scan(root, options, [&](const FileRecord& r) {
            if (r.name == "alpha.txt") walked = r.modifiedTime;
        });
        DS_CHECK_EQ(walked, first);

        std::filesystem::last_write_time(root / "alpha.txt",
                                         std::filesystem::file_time_type(whole) + milliseconds(500));
        DS_CHECK_EQ(FileScanner::statFile(root / "alpha.txt", options)->modifiedTime, first);
        std::filesystem::last_write_time(root / "alpha.txt", std::filesystem::file_time_type(whole) + seconds(1));
        DS_CHECK_EQ(FileScanner::statFile(root / "alpha.txt", options)->modifiedTime, first + 1);
        std::filesystem::last_write_time(root / "alpha.txt",
                                         std::filesystem::file_time_type(whole) - milliseconds(1));
        DS_CHECK_EQ(FileScanner::statFile(root / "alpha.txt", options)->modifiedTime, first - 1);
    }

    DS_CHECK(!FileScanner::statFile(root / "does_not_exist.txt", options).has_value());
    DS_CHECK(!FileScanner::statFile(root / "skip.tmp", options).has_value());  // excluded by mask

    // isExcluded: a live change deep inside an excluded folder is skipped,
    // as the full walk would skip it; only folders below the root count.
    {
        ScanOptions masks;
        masks.excludeMasks = {"node_modules", ".git", "*.tmp"};
        DS_CHECK(FileScanner::isExcluded("/r", "/r/app/node_modules/lib/x.js", masks));
        DS_CHECK(FileScanner::isExcluded("/r", "/r/app/cache.TMP", masks));
        DS_CHECK(!FileScanner::isExcluded("/r", "/r/app/readme.txt", masks));
        DS_CHECK(!FileScanner::isExcluded("/r", "/r/.gitignore", masks));
        DS_CHECK(FileScanner::isExcluded("D:\\", "D:\\work\\.git\\HEAD", masks));
        DS_CHECK(!FileScanner::isExcluded("/r/node_modules/proj", "/r/node_modules/proj/a.txt", masks));
        DS_CHECK(!FileScanner::isExcluded("/r", "/r/app/node_modules/x.js", ScanOptions{}));
        // An Office owner file ("~$" + the open document's name) never counts.
        DS_CHECK(FileScanner::isExcluded("/r", "/r/app/~$report.docx", ScanOptions{}));
        DS_CHECK(!FileScanner::isExcluded("/r", "/r/app/~report.docx", ScanOptions{}));
    }
    {
        writeFile(root / "office" / "report.docx", "doc");
        writeFile(root / "office" / "~$report.docx", "lock");
        std::size_t seen = 0;
        bool ownerSeen = false;
        FileScanner::scan(root / "office", {}, [&](const FileRecord& r) {
            ++seen;
            ownerSeen = ownerSeen || r.name.rfind("~$", 0) == 0;
        });
        DS_CHECK_EQ(seen, std::size_t{1});
        DS_CHECK(!ownerSeen);
        DS_CHECK(!FileScanner::statFile(root / "office" / "~$report.docx", {}).has_value());
    }

    // isAccessible: used to detect a temporarily unreachable source
    // (ТЗ п.11.4) before treating "nothing found" as "everything deleted".
    DS_CHECK(FileScanner::isAccessible(root));
    DS_CHECK(!FileScanner::isAccessible(root / "does_not_exist_dir"));
    DS_CHECK(!FileScanner::isAccessible(root / "alpha.txt"));  // a file, not a directory
}
