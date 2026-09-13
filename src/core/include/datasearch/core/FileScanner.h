#pragma once

#include "datasearch/core/FileRecord.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace datasearch::core {

struct ScanOptions {
    // Simple '*'/'?' masks (like .gitignore-lite, ТЗ FR-8), matched
    // case-insensitively against a file/directory *name* (not the full path).
    // A directory matching a mask is not descended into.
    std::vector<std::string> excludeMasks;

    // std::filesystem has no portable "creation time" API, so the platform
    // layer (Win32: GetFileAttributesExW) supplies one here when available.
    // Left unset, createdTime on scanned records stays 0.
    std::function<std::int64_t(const std::filesystem::path&)> creationTimeProvider;
};

using FileVisitor = std::function<void(const FileRecord&)>;
using DirectoryVisitor = std::function<void(const std::filesystem::path&)>;

class FileScanner {
public:
    // Recursively walks `root`, invoking `visitor` once per regular file:
    // a directory's files first, then its subdirectories, depth-first.
    // Symlinks (and, on Windows, junctions) are not followed — avoids cycles.
    // A directory that can't be read — no permission, path too long, removed
    // mid-walk, I/O error — or whose listing breaks off partway is reported
    // to `onUnreadableDirectory` and skipped; the rest of the walk carries on.
    // `cancelled`, if non-null, is polled between entries so a caller can
    // stop a long-running scan early.
    static void scan(const std::filesystem::path& root,
                      const ScanOptions& options,
                      const FileVisitor& visitor,
                      const std::atomic<bool>* cancelled = nullptr,
                      const DirectoryVisitor& onUnreadableDirectory = {});

    // Metadata for exactly one file, without walking the rest of the tree —
    // used to apply a single live filesystem-change event (ТЗ FR-6/п.13.2)
    // cheaply, instead of re-scanning the whole source. Returns std::nullopt
    // if `path` doesn't exist, isn't a regular file, or matches an exclude
    // mask in `options` (by filename, same rule as scan()).
    static std::optional<FileRecord> statFile(const std::filesystem::path& path,
                                               const ScanOptions& options = {});

    // Whether scan() of `root` would skip `path` because it or a folder on
    // the way to it (below `root`) matches an exclude mask — for a single
    // live change, which statFile() alone would check by file name only.
    static bool isExcluded(const std::string& root, const std::string& path, const ScanOptions& options);

    // Whether `root` currently resolves to an accessible directory. Used
    // before scanning/reconciling a source so a temporarily unreachable
    // network drive (ТЗ п.11.4: "диск недоступен") doesn't get treated as
    // "every indexed file was deleted" — see Indexer.
    static bool isAccessible(const std::filesystem::path& root);
};

} // namespace datasearch::core
