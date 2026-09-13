#include "datasearch/core/FileScanner.h"

#include "datasearch/core/Utf8.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <system_error>

namespace datasearch::core {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Minimal '*'/'?' glob matcher, case-insensitive. '*' matches any run of
// characters (including empty), '?' matches exactly one character.
bool matchesGlob(const std::string& pattern, const std::string& text) {
    const std::string p = toLower(pattern);
    const std::string t = toLower(text);

    std::vector<std::vector<char>> dp(p.size() + 1, std::vector<char>(t.size() + 1, 0));
    dp[0][0] = 1;
    for (std::size_t i = 1; i <= p.size(); ++i) {
        if (p[i - 1] == '*') dp[i][0] = dp[i - 1][0];
    }
    for (std::size_t i = 1; i <= p.size(); ++i) {
        for (std::size_t j = 1; j <= t.size(); ++j) {
            if (p[i - 1] == '*') {
                dp[i][j] = dp[i - 1][j] || dp[i][j - 1];
            } else if (p[i - 1] == '?' || p[i - 1] == t[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1];
            }
        }
    }
    return dp[p.size()][t.size()] != 0;
}

bool matchesAnyMask(const std::vector<std::string>& masks, const std::string& name) {
    for (const auto& mask : masks) {
        if (matchesGlob(mask, name)) return true;
    }
    return false;
}

std::int64_t toEpochSeconds(std::filesystem::file_time_type ftime) {
    using namespace std::chrono;
    // Avoid file_clock::to_sys(): at least one MSVC preview toolset (used by
    // GitHub Actions' windows-latest runner as of writing) fails to compile
    // it, complaining that to_sys isn't a member of the internal clock type
    // file_clock aliases to. Converting via a now()/now() offset instead only
    // relies on basic time_point arithmetic, which every standard clock
    // supports regardless of whether it also defines to_sys/from_sys.
    const auto sctp = time_point_cast<system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + system_clock::now());
    return static_cast<std::int64_t>(duration_cast<seconds>(sctp.time_since_epoch()).count());
}

} // namespace

// An explicit stack of directories rather than recursive_directory_iterator:
// that iterator ends the *entire* walk at the first error it can't skip (a
// path over the OS limit, a directory deleted mid-walk, a sharing
// violation). Everything after that point silently went unseen — files never
// got indexed, and a reconcile pass then dropped indexed files it hadn't
// reached as "deleted". With transient errors the cut-off moved from run to
// run, so tens of thousands of files were removed and re-added every start.
void FileScanner::scan(const std::filesystem::path& root,
                        const ScanOptions& options,
                        const FileVisitor& visitor,
                        const std::atomic<bool>* cancelled,
                        const DirectoryVisitor& onUnreadableDirectory) {
    auto isCancelled = [cancelled] { return cancelled != nullptr && cancelled->load(std::memory_order_relaxed); };
    auto reportUnreadable = [&](const std::filesystem::path& dir) {
        if (onUnreadableDirectory) onUnreadableDirectory(dir);
    };

    std::vector<std::filesystem::path> pending{root};
    while (!pending.empty()) {
        if (isCancelled()) return;
        const std::filesystem::path dir = std::move(pending.back());
        pending.pop_back();

        std::error_code iterEc;
        // No skip_permission_denied: a folder we may not read is as unknown
        // as one that failed any other way, and must be reported so a
        // reconcile pass keeps (rather than drops) what was indexed in it.
        std::filesystem::directory_iterator it(dir, iterEc);
        if (iterEc) {
            reportUnreadable(dir);
            continue;
        }

        std::vector<std::filesystem::path> subdirs;
        const std::filesystem::directory_iterator end;
        for (; it != end; it.increment(iterEc)) {
            if (isCancelled()) return;

            const std::filesystem::directory_entry& entry = *it;
            // An entry that can't even be stat'ed (e.g. its path exceeds the OS
            // limit) may well be a directory; report it rather than let
            // whatever it holds look deleted.
            std::error_code ec;
            const auto linkType = entry.symlink_status(ec).type();
            if (ec) {
                reportUnreadable(entry.path());
                continue;
            }
            bool isLink = linkType == std::filesystem::file_type::symlink;
#if defined(_MSC_VER)
            isLink = isLink || linkType == std::filesystem::file_type::junction;
#endif
            if (isLink) continue;

            const std::string name = pathToUtf8(entry.path().filename());
            const bool isDir = entry.is_directory(ec);
            if (ec) {
                reportUnreadable(entry.path());
                continue;
            }
            if (isDir) {
                if (!matchesAnyMask(options.excludeMasks, name)) subdirs.push_back(entry.path());
                continue;
            }

            const bool isRegular = entry.is_regular_file(ec);
            if (ec || !isRegular) continue;
            if (matchesAnyMask(options.excludeMasks, name)) continue;

            FileRecord record;
            record.path = pathToUtf8(entry.path());
            record.name = name;
            record.extension = pathToUtf8(entry.path().extension());

            record.size = static_cast<std::uint64_t>(entry.file_size(ec));
            if (ec) {
                ec.clear();
                record.size = 0;
            }

            const auto mtime = entry.last_write_time(ec);
            if (!ec) {
                record.modifiedTime = toEpochSeconds(mtime);
            }

            if (options.creationTimeProvider) {
                record.createdTime = options.creationTimeProvider(entry.path());
            }

            visitor(record);
        }
        // The listing broke off partway: whatever came after is unknown.
        if (iterEc) reportUnreadable(dir);

        // Pushed in reverse so they're popped — and walked — in listing order.
        for (auto sub = subdirs.rbegin(); sub != subdirs.rend(); ++sub) pending.push_back(std::move(*sub));
    }
}

std::optional<FileRecord> FileScanner::statFile(const std::filesystem::path& path,
                                                 const ScanOptions& options) {
    std::error_code ec;

    const bool isRegular = std::filesystem::is_regular_file(path, ec);
    if (ec || !isRegular) return std::nullopt;

    const std::string name = pathToUtf8(path.filename());
    if (matchesAnyMask(options.excludeMasks, name)) return std::nullopt;

    FileRecord record;
    record.path = pathToUtf8(path);
    record.name = name;
    record.extension = pathToUtf8(path.extension());

    record.size = static_cast<std::uint64_t>(std::filesystem::file_size(path, ec));
    if (ec) {
        ec.clear();
        record.size = 0;
    }

    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (!ec) {
        record.modifiedTime = toEpochSeconds(mtime);
    }
    ec.clear();

    if (options.creationTimeProvider) {
        record.createdTime = options.creationTimeProvider(path);
    }

    return record;
}

bool FileScanner::isAccessible(const std::filesystem::path& root) {
    std::error_code ec;
    const bool isDir = std::filesystem::is_directory(root, ec);
    return !ec && isDir;
}

} // namespace datasearch::core
