#pragma once

#include "datasearch/core/FileRecord.h"

#include <atomic>
#include <filesystem>
#include <functional>
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

class FileScanner {
public:
    // Recursively walks `root`, invoking `visitor` once per regular file.
    // Symlinks are not followed (avoids cycles). Entries that raise a
    // filesystem error (permission denied, broken reparse point, ...) are
    // skipped rather than aborting the whole scan.
    // `cancelled`, if non-null, is polled between entries so a caller can
    // stop a long-running scan early.
    static void scan(const std::filesystem::path& root,
                      const ScanOptions& options,
                      const FileVisitor& visitor,
                      const std::atomic<bool>* cancelled = nullptr);
};

} // namespace datasearch::core
