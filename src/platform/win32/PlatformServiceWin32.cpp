// Windows implementation of IPlatformService (ТЗ п.9/11.1/12.1 — the only
// platform implementation shipped in stage 1). Only ever compiled on Windows
// (see ../CMakeLists.txt), so this file is free to use Win32 headers directly.

#include "datasearch/platform/IPlatformService.h"

#include "datasearch/core/Utf8.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <array>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

using datasearch::core::pathToUtf8;

namespace datasearch::platform {

namespace {

class ComScope {
public:
    ComScope() { hr_ = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComScope() {
        if (SUCCEEDED(hr_)) ::CoUninitialize();
    }

private:
    HRESULT hr_ = E_FAIL;
};

VolumeType mapDriveType(UINT winType) {
    switch (winType) {
        case DRIVE_FIXED: return VolumeType::Local;
        case DRIVE_REMOVABLE: return VolumeType::Removable;
        case DRIVE_CDROM: return VolumeType::Removable;
        case DRIVE_RAMDISK: return VolumeType::Local;
        case DRIVE_REMOTE: return VolumeType::Network;
        default: return VolumeType::Unknown;
    }
}

// One recursive ReadDirectoryChangesW watch running on its own thread, using
// overlapped I/O so it can be cancelled cleanly (CancelIoEx) instead of
// blocking forever in a synchronous read (ТЗ FR-6 / п.13.2).
class DirectoryWatchWin32 : public IDirectoryWatch {
public:
    DirectoryWatchWin32(const std::filesystem::path& root, FileSystemChangeCallback onChange)
        : root_(root), onChange_(std::move(onChange)) {
        const std::wstring wroot = root.wstring();
        dirHandle_ = ::CreateFileW(wroot.c_str(), FILE_LIST_DIRECTORY,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                    nullptr);
        if (dirHandle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to open directory for watching");
        }
        stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        overlapped_.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

        worker_ = std::thread(&DirectoryWatchWin32::run, this);
    }

    ~DirectoryWatchWin32() override {
        ::SetEvent(stopEvent_);
        ::CancelIoEx(dirHandle_, &overlapped_);
        if (worker_.joinable()) worker_.join();
        ::CloseHandle(overlapped_.hEvent);
        ::CloseHandle(stopEvent_);
        ::CloseHandle(dirHandle_);
    }

private:
    static constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME |
                                            FILE_NOTIFY_CHANGE_DIR_NAME |
                                            FILE_NOTIFY_CHANGE_LAST_WRITE |
                                            FILE_NOTIFY_CHANGE_SIZE |
                                            FILE_NOTIFY_CHANGE_CREATION;

    void run() {
        std::vector<BYTE> buffer(64 * 1024);

        while (true) {
            DWORD bytesReturned = 0;
            ::ResetEvent(overlapped_.hEvent);
            BOOL ok = ::ReadDirectoryChangesW(dirHandle_, buffer.data(),
                                               static_cast<DWORD>(buffer.size()), /*bWatchSubtree=*/TRUE,
                                               kNotifyFilter, &bytesReturned, &overlapped_, nullptr);
            if (!ok && ::GetLastError() != ERROR_IO_PENDING) {
                return;
            }

            std::array<HANDLE, 2> handles{overlapped_.hEvent, stopEvent_};
            const DWORD waitResult = ::WaitForMultipleObjects(2, handles.data(), FALSE, INFINITE);
            if (waitResult == WAIT_OBJECT_0 + 1) {
                return;  // stop requested
            }

            DWORD transferred = 0;
            if (!::GetOverlappedResult(dirHandle_, &overlapped_, &transferred, FALSE) ||
                transferred == 0) {
                continue;
            }

            std::size_t offset = 0;
            while (offset < transferred) {
                const auto* info =
                    reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
                const std::wstring name(info->FileName, info->FileNameLength / sizeof(WCHAR));

                FileSystemChange change;
                change.path = root_ / name;
                switch (info->Action) {
                    case FILE_ACTION_ADDED: change.kind = FileSystemChange::Kind::Created; break;
                    case FILE_ACTION_REMOVED: change.kind = FileSystemChange::Kind::Removed; break;
                    case FILE_ACTION_MODIFIED: change.kind = FileSystemChange::Kind::Modified; break;
                    case FILE_ACTION_RENAMED_NEW_NAME:
                        change.kind = FileSystemChange::Kind::RenamedTo;
                        break;
                    case FILE_ACTION_RENAMED_OLD_NAME:
                        change.kind = FileSystemChange::Kind::Removed;
                        break;
                    default: change.kind = FileSystemChange::Kind::Modified; break;
                }
                if (onChange_) onChange_(change);

                if (info->NextEntryOffset == 0) break;
                offset += info->NextEntryOffset;
            }
        }
    }

    std::filesystem::path root_;
    HANDLE dirHandle_ = INVALID_HANDLE_VALUE;
    HANDLE stopEvent_ = nullptr;
    OVERLAPPED overlapped_{};
    std::thread worker_;
    FileSystemChangeCallback onChange_;
};

class PlatformServiceWin32 : public IPlatformService {
public:
    std::vector<VolumeInfo> enumerateVolumes() override {
        std::vector<VolumeInfo> volumes;
        const DWORD driveMask = ::GetLogicalDrives();

        for (int i = 0; i < 26; ++i) {
            if ((driveMask & (1u << i)) == 0) continue;

            const wchar_t letter = static_cast<wchar_t>(L'A' + i);
            const std::wstring root = std::wstring(1, letter) + L":\\";

            const UINT winType = ::GetDriveTypeW(root.c_str());
            if (winType == DRIVE_UNKNOWN || winType == DRIVE_NO_ROOT_DIR) continue;

            VolumeInfo info;
            info.rootPath = pathToUtf8(std::filesystem::path(root));
            info.type = mapDriveType(winType);

            wchar_t labelBuf[MAX_PATH + 1] = {};
            if (::GetVolumeInformationW(root.c_str(), labelBuf, MAX_PATH, nullptr, nullptr, nullptr,
                                         nullptr, 0)) {
                info.label = pathToUtf8(std::filesystem::path(labelBuf));
            }

            ULARGE_INTEGER freeBytes{};
            ULARGE_INTEGER totalBytes{};
            if (::GetDiskFreeSpaceExW(root.c_str(), &freeBytes, &totalBytes, nullptr)) {
                info.freeBytes = freeBytes.QuadPart;
                info.totalBytes = totalBytes.QuadPart;
            }

            volumes.push_back(std::move(info));
        }

        return volumes;
    }

    void showInFolder(const std::filesystem::path& file) override {
        ComScope com;
        PIDLIST_ABSOLUTE pidl = ::ILCreateFromPathW(file.wstring().c_str());
        if (pidl == nullptr) {
            throw std::runtime_error("Failed to resolve path for Explorer selection");
        }
        ::SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ::ILFree(pidl);
    }

    void openFile(const std::filesystem::path& file) override {
        const std::wstring wpath = file.wstring();
        const HINSTANCE result =
            ::ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            throw std::runtime_error("Failed to open file with the associated application");
        }
    }

    std::unique_ptr<IDirectoryWatch> watchDirectory(const std::filesystem::path& root,
                                                      FileSystemChangeCallback onChange) override {
        return std::make_unique<DirectoryWatchWin32>(root, std::move(onChange));
    }

    void lowerCurrentThreadPriority() override {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }
};

} // namespace

std::unique_ptr<IPlatformService> createPlatformService() {
    return std::make_unique<PlatformServiceWin32>();
}

} // namespace datasearch::platform
