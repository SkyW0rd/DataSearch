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

void FileScanner::scan(const std::filesystem::path& root,
                        const ScanOptions& options,
                        const FileVisitor& visitor,
                        const std::atomic<bool>* cancelled) {
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;

    for (; it != end; it.increment(ec)) {
        if (cancelled != nullptr && cancelled->load(std::memory_order_relaxed)) {
            return;
        }
        if (ec) {
            // Skip the entry that raised the error and keep going.
            ec.clear();
            continue;
        }

        const std::filesystem::directory_entry& entry = *it;
        const std::string name = pathToUtf8(entry.path().filename());

        if (entry.is_symlink(ec)) {
            it.disable_recursion_pending();
            continue;
        }

        bool isDir = entry.is_directory(ec);
        if (ec) {
            ec.clear();
            continue;
        }

        if (isDir) {
            if (matchesAnyMask(options.excludeMasks, name)) {
                it.disable_recursion_pending();
            }
            continue;
        }

        bool isRegular = entry.is_regular_file(ec);
        if (ec || !isRegular) {
            ec.clear();
            continue;
        }

        if (matchesAnyMask(options.excludeMasks, name)) {
            continue;
        }

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
        ec.clear();

        if (options.creationTimeProvider) {
            record.createdTime = options.creationTimeProvider(entry.path());
        }

        visitor(record);
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
