// macOS implementation of IPlatformService (ТЗ п.9/11.1, этап 6). Built and
// tested on this machine (unlike the Windows implementation, which can only
// be verified on a real Windows box per the project's dev workflow).

#include "datasearch/platform/IPlatformService.h"

#include "datasearch/core/Utf8.h"

#import <Cocoa/Cocoa.h>
#import <CoreServices/CoreServices.h>

#include <pthread.h>
#include <pthread/qos.h>
#include <sys/mount.h>
#include <sys/param.h>

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

using datasearch::core::pathFromUtf8;
using datasearch::core::pathToUtf8;

namespace datasearch::platform {

namespace {

VolumeType classifyMount(const struct statfs& m, const std::string& rootPath) {
    if (!(m.f_flags & MNT_LOCAL)) return VolumeType::Network;
    // macOS doesn't expose "removable" on struct statfs directly (that needs
    // DiskArbitration + a CFRunLoop session, out of scope here); external and
    // removable media almost always mount under /Volumes/, while the boot
    // volume and other fixed internal volumes mount at "/" — a reasonable,
    // documented heuristic rather than a hardware-verified answer.
    if (rootPath.rfind("/Volumes/", 0) == 0) return VolumeType::Removable;
    return VolumeType::Local;
}

// One recursive FSEvents watch running on its own thread with its own
// CFRunLoop (the standard pattern for using FSEvents outside an AppKit main
// run loop). File-level granularity (kFSEventStreamCreateFlagFileEvents)
// gives per-file created/removed/modified/renamed flags directly, matching
// what ReadDirectoryChangesW gives on Windows (ТЗ FR-6/п.13.2).
class DirectoryWatchMacOS : public IDirectoryWatch {
public:
    DirectoryWatchMacOS(const std::filesystem::path& root, FileSystemChangeCallback onChange)
        : root_(root), onChange_(std::move(onChange)) {
        thread_ = std::thread(&DirectoryWatchMacOS::run, this);

        std::unique_lock<std::mutex> lock(startMutex_);
        startCv_.wait(lock, [this] { return runLoop_ != nullptr; });
    }

    ~DirectoryWatchMacOS() override {
        if (runLoop_ != nullptr) {
            CFRunLoopStop(runLoop_);
        }
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        @autoreleasepool {
            NSString* pathStr = [NSString stringWithUTF8String:pathToUtf8(root_).c_str()];
            CFArrayRef pathsToWatch = (__bridge CFArrayRef) @[ pathStr ];

            FSEventStreamContext context{};
            context.info = this;

            stream_ = FSEventStreamCreate(kCFAllocatorDefault, &DirectoryWatchMacOS::callback, &context,
                                           pathsToWatch, kFSEventStreamEventIdSinceNow,
                                           /*latencySeconds=*/0.3,
                                           kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
            if (stream_ == nullptr) {
                std::lock_guard<std::mutex> lock(startMutex_);
                runLoop_ = CFRunLoopGetCurrent();  // unblock the constructor even on failure
                startCv_.notify_all();
                return;
            }

            {
                std::lock_guard<std::mutex> lock(startMutex_);
                runLoop_ = CFRunLoopGetCurrent();
            }
            FSEventStreamScheduleWithRunLoop(stream_, runLoop_, kCFRunLoopDefaultMode);
            FSEventStreamStart(stream_);
            startCv_.notify_all();

            CFRunLoopRun();

            FSEventStreamStop(stream_);
            FSEventStreamInvalidate(stream_);
            FSEventStreamRelease(stream_);
            stream_ = nullptr;
        }
    }

    static void callback(ConstFSEventStreamRef, void* clientInfo, std::size_t numEvents, void* eventPathsRaw,
                          const FSEventStreamEventFlags eventFlags[], const FSEventStreamEventId[]) {
        auto* self = static_cast<DirectoryWatchMacOS*>(clientInfo);
        auto** eventPaths = static_cast<char**>(eventPathsRaw);

        for (std::size_t i = 0; i < numEvents; ++i) {
            if (!(eventFlags[i] & kFSEventStreamEventFlagItemIsFile)) continue;

            FileSystemChange change;
            change.path = pathFromUtf8(eventPaths[i]);
            if (eventFlags[i] & kFSEventStreamEventFlagItemRemoved) {
                change.kind = FileSystemChange::Kind::Removed;
            } else if (eventFlags[i] & kFSEventStreamEventFlagItemRenamed) {
                change.kind = FileSystemChange::Kind::RenamedTo;
            } else if (eventFlags[i] & kFSEventStreamEventFlagItemCreated) {
                change.kind = FileSystemChange::Kind::Created;
            } else {
                change.kind = FileSystemChange::Kind::Modified;
            }

            if (self->onChange_) self->onChange_(change);
        }
    }

    std::filesystem::path root_;
    FileSystemChangeCallback onChange_;
    std::thread thread_;
    std::mutex startMutex_;
    std::condition_variable startCv_;
    CFRunLoopRef runLoop_ = nullptr;
    FSEventStreamRef stream_ = nullptr;
};

class PlatformServiceMacOS : public IPlatformService {
public:
    std::vector<VolumeInfo> enumerateVolumes() override {
        std::vector<VolumeInfo> volumes;

        struct statfs* mounts = nullptr;
        const int count = getmntinfo(&mounts, MNT_NOWAIT);  // kernel-owned buffer, not freed by us
        for (int i = 0; i < count; ++i) {
            const struct statfs& m = mounts[i];
            const std::string fsType = m.f_fstypename;
            if (fsType == "devfs" || fsType == "autofs" || fsType == "fdesc") continue;

            const std::string mountPoint = m.f_mntonname;
            // Finder only ever shows the boot volume ("/") and anything under
            // "/Volumes/". Modern macOS additionally mounts several hidden
            // APFS system-role volumes under "/System/Volumes/" (VM, Preboot,
            // Update, xarts, iSCPreboot, Hardware, plus Data — which is what
            // "/" itself actually resolves to via a firmlink) that a user
            // never sees and would never want offered as an index source —
            // observed and filtered out on this machine, not just assumed.
            const bool isBootVolume = mountPoint == "/";
            const bool isUserVolume = mountPoint.rfind("/Volumes/", 0) == 0;
            if (!isBootVolume && !isUserVolume) continue;

            VolumeInfo info;
            info.rootPath = mountPoint;
            info.type = classifyMount(m, info.rootPath);

            const auto lastSlash = info.rootPath.find_last_of('/');
            if (info.rootPath != "/" && lastSlash != std::string::npos) {
                info.label = info.rootPath.substr(lastSlash + 1);
            }

            info.totalBytes = static_cast<std::uint64_t>(m.f_blocks) * static_cast<std::uint64_t>(m.f_bsize);
            info.freeBytes = static_cast<std::uint64_t>(m.f_bavail) * static_cast<std::uint64_t>(m.f_bsize);

            volumes.push_back(std::move(info));
        }

        return volumes;
    }

    void showInFolder(const std::filesystem::path& file) override {
        @autoreleasepool {
            NSString* path = [NSString stringWithUTF8String:pathToUtf8(file).c_str()];
            NSString* parent = [path stringByDeletingLastPathComponent];
            const BOOL ok = [[NSWorkspace sharedWorkspace] selectFile:path inFileViewerRootedAtPath:parent];
            if (!ok) {
                throw std::runtime_error("Failed to reveal file in Finder");
            }
        }
    }

    void openFile(const std::filesystem::path& file) override {
        @autoreleasepool {
            NSString* path = [NSString stringWithUTF8String:pathToUtf8(file).c_str()];
            NSURL* url = [NSURL fileURLWithPath:path];
            const BOOL ok = [[NSWorkspace sharedWorkspace] openURL:url];
            if (!ok) {
                throw std::runtime_error("Failed to open file with the associated application");
            }
        }
    }

    std::unique_ptr<IDirectoryWatch> watchDirectory(const std::filesystem::path& root,
                                                      FileSystemChangeCallback onChange) override {
        return std::make_unique<DirectoryWatchMacOS>(root, std::move(onChange));
    }

    void lowerCurrentThreadPriority() override {
        // QOS_CLASS_UTILITY: the documented modern macOS analogue of Windows'
        // THREAD_PRIORITY_BELOW_NORMAL — background work the user isn't
        // directly waiting on (ТЗ п.12.3).
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    }
};

} // namespace

std::unique_ptr<IPlatformService> createPlatformService() {
    return std::make_unique<PlatformServiceMacOS>();
}

} // namespace datasearch::platform
