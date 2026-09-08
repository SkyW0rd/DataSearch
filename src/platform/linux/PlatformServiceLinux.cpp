// Linux implementation of IPlatformService (ТЗ п.9/11.1, этап 6). Written
// carefully against documented Linux APIs but — unlike the macOS
// implementation next to it — never compiled or run on a real Linux machine
// (none available in this project's dev environment); treat it the same way
// the Windows implementation was treated for stages 1-5: real code, unverified
// until it's actually built and exercised on the target OS.

#include "datasearch/platform/IPlatformService.h"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

namespace datasearch::platform {

namespace {

// /proc/mounts octal-escapes space, tab, backslash and newline in paths
// (e.g. "/media/user/My\040Drive").
std::string unescapeMountPath(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isdigit(static_cast<unsigned char>(s[i + 2])) &&
            std::isdigit(static_cast<unsigned char>(s[i + 3]))) {
            const int value = (s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0');
            out.push_back(static_cast<char>(value));
            i += 3;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// Launches `command arg` detached (double-fork, so the child is reparented
// to init and we never accumulate zombies without a SIGCHLD handler). Only
// verifies the intermediate fork succeeded, not that `command` itself was
// found/succeeded — an inherent limitation of a fire-and-forget launch.
void runDetached(const char* command, const std::string& arg) {
    const pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error(std::string("fork() failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
        if (fork() == 0) {
            execlp(command, command, arg.c_str(), static_cast<char*>(nullptr));
            _exit(127);  // execlp only returns on failure
        }
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error(std::string("Failed to launch ") + command);
    }
}

// Recursive inotify watch: inotify itself only watches one directory level,
// so this walks the tree at start-up adding one watch per directory, and
// extends coverage as new subdirectories are created (ТЗ FR-6/п.13.2 — the
// same recursive semantics ReadDirectoryChangesW/FSEvents give directly).
class DirectoryWatchLinux : public IDirectoryWatch {
public:
    DirectoryWatchLinux(const std::filesystem::path& root, FileSystemChangeCallback onChange)
        : root_(root), onChange_(std::move(onChange)) {
        inotifyFd_ = inotify_init1(IN_NONBLOCK);
        if (inotifyFd_ < 0) {
            throw std::runtime_error(std::string("inotify_init1 failed: ") + std::strerror(errno));
        }
        stopFd_ = eventfd(0, EFD_NONBLOCK);
        if (stopFd_ < 0) {
            close(inotifyFd_);
            throw std::runtime_error(std::string("eventfd failed: ") + std::strerror(errno));
        }

        addWatchesRecursive(root_);
        thread_ = std::thread(&DirectoryWatchLinux::run, this);
    }

    ~DirectoryWatchLinux() override {
        const std::uint64_t one = 1;
        if (write(stopFd_, &one, sizeof(one)) < 0) {
            // Best-effort wake-up; if this fails the thread will still exit
            // once poll() is interrupted or the fds are closed below.
        }
        if (thread_.joinable()) thread_.join();
        close(inotifyFd_);
        close(stopFd_);
    }

private:
    static constexpr std::uint32_t kWatchMask = IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM |
                                                 IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;

    void addWatchesRecursive(const std::filesystem::path& dir) {
        const int wd = inotify_add_watch(inotifyFd_, dir.c_str(), kWatchMask);
        if (wd >= 0) {
            std::lock_guard<std::mutex> lock(watchesMutex_);
            watchToPath_[wd] = dir;
        }

        std::error_code ec;
        std::filesystem::directory_iterator it(
            dir, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::directory_iterator end;
        for (; it != end && !ec; it.increment(ec)) {
            std::error_code entryEc;
            if (it->is_symlink(entryEc)) continue;
            if (it->is_directory(entryEc) && !entryEc) {
                addWatchesRecursive(it->path());
            }
        }
    }

    void run() {
        std::vector<char> buffer(64 * 1024);
        struct pollfd fds[2];
        fds[0].fd = inotifyFd_;
        fds[0].events = POLLIN;
        fds[1].fd = stopFd_;
        fds[1].events = POLLIN;

        while (true) {
            const int rc = poll(fds, 2, -1);
            if (rc < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (fds[1].revents & POLLIN) return;  // stop requested

            if (fds[0].revents & POLLIN) {
                const ssize_t n = read(inotifyFd_, buffer.data(), buffer.size());
                if (n <= 0) continue;

                std::size_t offset = 0;
                while (offset + sizeof(struct inotify_event) <= static_cast<std::size_t>(n)) {
                    const auto* event = reinterpret_cast<const struct inotify_event*>(buffer.data() + offset);
                    handleEvent(event);
                    offset += sizeof(struct inotify_event) + event->len;
                }
            }
        }
    }

    void handleEvent(const struct inotify_event* event) {
        if (event->mask & IN_IGNORED) {
            std::lock_guard<std::mutex> lock(watchesMutex_);
            watchToPath_.erase(event->wd);
            return;
        }
        if (event->len == 0) return;  // event about the watched directory itself

        std::filesystem::path dirPath;
        {
            std::lock_guard<std::mutex> lock(watchesMutex_);
            const auto it = watchToPath_.find(event->wd);
            if (it == watchToPath_.end()) return;
            dirPath = it->second;
        }

        const std::filesystem::path itemPath = dirPath / std::string(event->name);

        if (event->mask & IN_ISDIR) {
            // inotify isn't recursive: pick up newly created subdirectories
            // so files created inside them are seen too.
            if (event->mask & IN_CREATE) addWatchesRecursive(itemPath);
            return;
        }

        FileSystemChange change;
        change.path = itemPath;
        if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
            change.kind = FileSystemChange::Kind::Removed;
        } else if (event->mask & IN_MOVED_TO) {
            change.kind = FileSystemChange::Kind::RenamedTo;
        } else if (event->mask & IN_CREATE) {
            change.kind = FileSystemChange::Kind::Created;
        } else {
            change.kind = FileSystemChange::Kind::Modified;
        }

        if (onChange_) onChange_(change);
    }

    std::filesystem::path root_;
    FileSystemChangeCallback onChange_;
    int inotifyFd_ = -1;
    int stopFd_ = -1;
    std::thread thread_;
    std::mutex watchesMutex_;
    std::unordered_map<int, std::filesystem::path> watchToPath_;
};

class PlatformServiceLinux : public IPlatformService {
public:
    std::vector<VolumeInfo> enumerateVolumes() override {
        std::vector<VolumeInfo> volumes;

        std::ifstream mounts("/proc/mounts");
        std::string line;

        static const std::unordered_set<std::string> kRealFsTypes = {
            "ext2", "ext3", "ext4", "xfs", "btrfs", "ntfs", "ntfs3", "vfat",
            "exfat", "hfsplus", "f2fs", "reiserfs", "jfs", "zfs", "apfs",
        };
        static const std::unordered_set<std::string> kNetworkFsTypes = {
            "nfs", "nfs4", "cifs", "smbfs", "smb3", "fuse.sshfs", "sshfs", "fuse.rclone", "ceph",
        };

        while (std::getline(mounts, line)) {
            std::istringstream iss(line);
            std::string device;
            std::string mountPointEscaped;
            std::string fsType;
            std::string options;
            if (!(iss >> device >> mountPointEscaped >> fsType >> options)) continue;

            const bool isNetwork = kNetworkFsTypes.count(fsType) != 0;
            if (!isNetwork && kRealFsTypes.count(fsType) == 0) continue;

            const std::string mountPoint = unescapeMountPath(mountPointEscaped);

            // Mirrors what a desktop file manager actually shows: the root
            // filesystem, plus removable/network media under the
            // conventional auto-mount locations — not every bind mount or
            // system partition (/boot, /var, container overlays, ...)
            // /proc/mounts also lists.
            const bool isRoot = mountPoint == "/";
            const bool isMedia = mountPoint.rfind("/media/", 0) == 0 || mountPoint.rfind("/run/media/", 0) == 0;
            const bool isMnt = mountPoint.rfind("/mnt/", 0) == 0;
            if (!isRoot && !isMedia && !isMnt) continue;

            VolumeInfo info;
            info.rootPath = mountPoint;
            info.type = isNetwork ? VolumeType::Network : (isMedia ? VolumeType::Removable : VolumeType::Local);

            const auto lastSlash = mountPoint.find_last_of('/');
            if (!isRoot && lastSlash != std::string::npos) {
                info.label = mountPoint.substr(lastSlash + 1);
            }

            struct statvfs st {};
            if (statvfs(mountPoint.c_str(), &st) == 0) {
                info.totalBytes = static_cast<std::uint64_t>(st.f_blocks) * st.f_frsize;
                info.freeBytes = static_cast<std::uint64_t>(st.f_bavail) * st.f_frsize;
            }

            volumes.push_back(std::move(info));
        }

        return volumes;
    }

    void showInFolder(const std::filesystem::path& file) override {
        // No unified Linux API for "open a folder with one file selected"
        // (ТЗ п.11.1) — falls back to just opening the containing folder via
        // whatever the desktop's default file manager is.
        runDetached("xdg-open", file.parent_path().string());
    }

    void openFile(const std::filesystem::path& file) override { runDetached("xdg-open", file.string()); }

    std::unique_ptr<IDirectoryWatch> watchDirectory(const std::filesystem::path& root,
                                                      FileSystemChangeCallback onChange) override {
        return std::make_unique<DirectoryWatchLinux>(root, std::move(onChange));
    }

    void lowerCurrentThreadPriority() override {
        // On Linux, setpriority(PRIO_PROCESS, 0, ...) affects the *calling
        // thread's* nice value (each thread has its own scheduling identity
        // there), matching Windows' THREAD_PRIORITY_BELOW_NORMAL intent
        // (ТЗ п.12.3) without needing a thread handle.
        setpriority(PRIO_PROCESS, 0, 10);
    }
};

} // namespace

std::unique_ptr<IPlatformService> createPlatformService() {
    return std::make_unique<PlatformServiceLinux>();
}

} // namespace datasearch::platform
