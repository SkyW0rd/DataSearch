#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace datasearch::platform {

enum class VolumeType { Local, Removable, Network, Unknown };

// Mirrors what Explorer shows in "Этот компьютер" (ТЗ п.12.2): every volume
// visible to the OS, with the same letter/label the user already knows.
struct VolumeInfo {
    std::string rootPath;    // e.g. "C:\\" on Windows
    std::string label;       // volume label, or empty if unset
    VolumeType type = VolumeType::Unknown;
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;
};

struct FileSystemChange {
    enum class Kind { Created, Modified, Removed, RenamedTo };
    std::filesystem::path path;
    Kind kind;
};

using FileSystemChangeCallback = std::function<void(const FileSystemChange&)>;

// A single watch on one directory subtree. Destroying the handle stops watching.
class IDirectoryWatch {
public:
    virtual ~IDirectoryWatch() = default;
};

// Everything that has no portable implementation and must go through the OS.
// Per ТЗ п.9/11.1/12.1: only a Windows implementation (src/platform/win32) ships
// for stage 1 — this header just fixes the seam other platforms plug into later.
class IPlatformService {
public:
    virtual ~IPlatformService() = default;

    virtual std::vector<VolumeInfo> enumerateVolumes() = 0;

    // Opens the OS file browser with `file` selected (Windows: SHOpenFolderAndSelectItems).
    virtual void showInFolder(const std::filesystem::path& file) = 0;

    // Opens `file` in its associated application (Windows: ShellExecuteW "open").
    virtual void openFile(const std::filesystem::path& file) = 0;

    // Watches `root` recursively for filesystem changes, invoking `onChange` for
    // each one (ТЗ FR-6 / п.13.2, Windows: ReadDirectoryChangesW). The returned
    // handle must outlive the desired watch duration.
    virtual std::unique_ptr<IDirectoryWatch> watchDirectory(const std::filesystem::path& root,
                                                              FileSystemChangeCallback onChange) = 0;
};

// Constructs the platform implementation for the OS this binary was built for.
// Only defined by the Windows translation unit for stage 1 (see CMakeLists).
std::unique_ptr<IPlatformService> createPlatformService();

} // namespace datasearch::platform
