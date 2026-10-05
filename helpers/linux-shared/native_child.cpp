// native_child.cpp - interoperable reimplementation of Typeless'
// LinuxShared::NativeChild (x64 libInputHelper.so / libContextHelper.so /
// libUtilHelper.so). See native_child.h for the contract.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native_child.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <sstream>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "native_child_guard.h"

extern char **environ;

namespace LinuxShared {
namespace NativeChild {
namespace {

struct KindConfig {
    const char *binaryName;
    const char *envVar;
    const char *runtimeName;
    const char *socketName;
    const char *pidName;
    const char *stderrLogName;
};

const KindConfig kConfigs[3] = {
    {"typeless-atspi-helper", "TYPELESS_ATSPI_HELPER", "typeless-atspi", "helper.sock",
     "helper.pid", "helper.stderr.log"},
    {"typeless-ibus-engine", "TYPELESS_IBUS_ENGINE", "typeless-ibus", "inject.sock",
     "engine.pid", "engine.stderr.log"},
    {"typeless-voice-ime", "TYPELESS_VOICE_IME", "typeless-voice-ime", "inject.sock",
     "engine.pid", "engine.stderr.log"},
};

inline int Index(Kind kind)
{
    int i = static_cast<int>(kind);
    return (i >= 0 && i < 3) ? i : 0;
}

inline const KindConfig &Config(Kind kind)
{
    return kConfigs[Index(kind)];
}

std::mutex g_mutex;                               // guards everything below
int g_spawnedPid[3] = {-1, -1, -1};               // pid of the child we spawned
std::chrono::steady_clock::time_point g_nextSpawnAt[3];  // spawn throttle
bool g_nextSpawnAtSet[3] = {false, false, false};
volatile bool g_respawnStopped[3] = {false, false, false};

constexpr auto kSpawnThrottle = std::chrono::milliseconds(400);

// PR_SET_PDEATHSIG (which the helper installs) fires when the *thread* that
// forked the child exits, not the process. The original spawns from whatever
// thread called in, so a helper spawned from a short-lived worker thread dies
// with that thread. We therefore run posix_spawn on one long-lived, detached
// spawner thread (deliberately leaked; it lives as long as the process).
class SpawnerThread {
public:
    static SpawnerThread &Get()
    {
        static SpawnerThread *instance = new SpawnerThread();
        return *instance;
    }

    int Run(const std::function<int()> &job)
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!started_) {
            try {
                std::thread([this] { Loop(); }).detach();
                started_ = true;
            } catch (...) {
                lk.unlock();
                return job();  // could not create a thread: spawn inline
            }
        }
        job_ = &job;
        done_ = false;
        cv_.notify_all();
        cv_.wait(lk, [this] { return done_; });
        return result_;
    }

private:
    void Loop()
    {
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            cv_.wait(lk, [this] { return job_ != nullptr; });
            const std::function<int()> *job = job_;
            job_ = nullptr;
            result_ = (*job)();
            done_ = true;
            cv_.notify_all();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    const std::function<int()> *job_ = nullptr;
    bool started_ = false;
    bool done_ = false;
    int result_ = 0;
};

void SleepMs(int ms)
{
    ::poll(nullptr, 0, ms);
}

// Non-blocking reap of a pid we spawned. Clears the slot when it is gone.
void ReapSpawnedLocked(int idx)
{
    int pid = g_spawnedPid[idx];
    if (pid <= 0)
        return;
    int status = 0;
    int r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid || (r < 0 && errno == ECHILD))
        g_spawnedPid[idx] = -1;
}

std::string DirectoryOfPath(const std::string &path)
{
    size_t slash = path.rfind('/');
    if (slash == std::string::npos)
        return ".";
    return path.substr(0, slash);
}

std::string MatchBinaryCandidate(Kind kind, const std::string &candidate)
{
    if (candidate.empty())
        return std::string();
    int mode = (kind == Kind::kVoiceIme) ? F_OK : X_OK;
    if (::access(candidate.c_str(), mode) != 0)
        return std::string();
    return candidate;
}

// Anchor for dladdr(): any function defined in this translation unit.
std::string ResolveBinaryPath(Kind kind)
{
    const KindConfig &cfg = Config(kind);
    const char *env = ::getenv(cfg.envVar);
    if (env != nullptr && env[0] != '\0')
        return std::string(env);

    Dl_info info;
    std::memset(&info, 0, sizeof info);
    if (::dladdr(reinterpret_cast<void *>(&ResolveBinaryPath), &info) != 0 &&
        info.dli_fname != nullptr) {
        std::string libDir = DirectoryOfPath(std::string(info.dli_fname));
        std::string hit = MatchBinaryCandidate(kind, libDir + "/" + cfg.binaryName);
        if (!hit.empty())
            return hit;
        hit = MatchBinaryCandidate(kind, DirectoryOfPath(libDir) + "/" + cfg.binaryName);
        if (!hit.empty())
            return hit;
    }

    char exe[4096];
    ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return std::string();
    exe[n] = '\0';
    return MatchBinaryCandidate(kind, DirectoryOfPath(std::string(exe)) + "/" + cfg.binaryName);
}

int TryConnect(Kind kind)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    std::string path = SocketPath(kind);
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() > sizeof(addr.sun_path) - 1) {
        ::close(fd);
        return -1;
    }
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
    // 1 s send timeout (the original sets it after connect; setting it first
    // also bounds a connect() that would wait on a full backlog).
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int rc;
    do {
        rc = ::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof addr);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool WaitUntilConnectableLocked(Kind kind)
{
    int idx = Index(kind);
    int attempts = (kind == Kind::kAtspiHelper) ? 100 : 40;
    int delayMs = (kind == Kind::kAtspiHelper) ? 80 : 25;
    for (int i = 0; i < attempts; ++i) {
        int fd = TryConnect(kind);
        if (fd >= 0) {
            ::close(fd);
            return true;
        }
        int pid = g_spawnedPid[idx];
        if (pid > 0) {
            int status = 0;
            int r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid || (r < 0 && errno == ECHILD)) {
                g_spawnedPid[idx] = -1;
                return false;  // our child died before becoming connectable
            }
        }
        SleepMs(delayMs);
    }
    return false;
}

int ReadIntsFromFile(const std::string &path, int *first, int *second)
{
    std::ifstream in(path);
    if (!in)
        return 0;
    int a = 0, b = 0, got = 0;
    if (in >> a) {
        got = 1;
        if (second != nullptr && (in >> b))
            got = 2;
    }
    if (first)
        *first = a;
    if (second)
        *second = b;
    return got;
}

int ReadPidFile(Kind kind)
{
    int pid = 0;
    if (ReadIntsFromFile(PidFilePath(kind), &pid, nullptr) < 1)
        return -1;
    return pid;
}

int ReadPidFileParent(Kind kind)
{
    int pid = 0, ppid = 0;
    if (ReadIntsFromFile(PidFilePath(kind), &pid, &ppid) < 2)
        return -1;
    return ppid;
}

// SIGTERM, then up to 1.5 s for it to go away (reaping it when it is our
// child), then SIGKILL (+ up to 200 ms reap when it is our child).
void SignalAndWait(int pid, bool isOurChild)
{
    if (pid <= 0)
        return;
    ::kill(pid, SIGTERM);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    if (isOurChild) {
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            int r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid || (r < 0 && errno == ECHILD))
                return;
            if (::kill(pid, 0) != 0 && errno == ESRCH) {
                ::waitpid(pid, &status, WNOHANG);
                return;
            }
            SleepMs(40);
        }
        ::kill(pid, SIGKILL);
        auto killDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        for (;;) {
            int status = 0;
            int r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid)
                return;
            if (r < 0 && errno == ECHILD)
                return;
            if (std::chrono::steady_clock::now() >= killDeadline) {
                ::waitpid(pid, &status, WNOHANG);
                return;
            }
            SleepMs(20);
        }
    } else {
        while (std::chrono::steady_clock::now() < deadline) {
            if (::kill(pid, 0) != 0 && errno == ESRCH)
                return;
            SleepMs(40);
        }
        ::kill(pid, SIGKILL);
    }
}

std::vector<char *> ToArgvPointers(std::vector<std::string> &v)
{
    std::vector<char *> out;
    out.reserve(v.size() + 1);
    for (auto &s : v)
        out.push_back(&s[0]);
    out.push_back(nullptr);
    return out;
}

void ReportSpawnFailure(Kind kind)
{
    const KindConfig &cfg = Config(kind);
    std::string logPath = RuntimeDirectory(kind) + "/" + cfg.stderrLogName;
    std::ifstream in(logPath, std::ios::binary);
    if (!in)
        return;
    char buf[2048];
    in.read(buf, sizeof(buf) - 1);
    std::streamsize n = in.gcount();
    if (n <= 0)
        return;
    buf[n] = '\0';
    std::fprintf(stderr, "native_child: %s stderr: %s\n", cfg.binaryName, buf);
    std::string text(buf);
    if (text.find("undefined symbol") != std::string::npos ||
        text.find("GLIBC_") != std::string::npos) {
        std::fprintf(stderr,
                     "native_child: %s ABI mismatch. Rebuild on Ubuntu 22.04. Do not upgrade "
                     "host GLib or libc.\n",
                     cfg.binaryName);
    }
}

bool SpawnLocked(Kind kind)
{
    int idx = Index(kind);
    const KindConfig &cfg = Config(kind);
    ReapSpawnedLocked(idx);

    std::string path = ResolveBinaryPath(kind);
    if (path.empty())
        return false;

    char dir[512];
    if (TypelessNativeChildRuntimeDirectory(cfg.runtimeName, dir, sizeof dir) != 0)
        return false;
    if (::mkdir(dir, 0700) != 0 && errno != EEXIST)
        return false;
    ::chmod(dir, 0700);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_t *actionsPtr = nullptr;
    int logFd = -1, nullFd = -1;
    if (posix_spawn_file_actions_init(&actions) == 0) {
        actionsPtr = &actions;
        if (cfg.stderrLogName != nullptr) {
            std::string logPath = std::string(dir) + "/" + cfg.stderrLogName;
            logFd = ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (logFd >= 0) {
                posix_spawn_file_actions_adddup2(&actions, logFd, STDERR_FILENO);
                posix_spawn_file_actions_addclose(&actions, logFd);
            }
        }
        nullFd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (nullFd >= 0) {
            posix_spawn_file_actions_adddup2(&actions, nullFd, STDOUT_FILENO);
            posix_spawn_file_actions_addclose(&actions, nullFd);
        }
    }

    // argv
    bool usePath = false;
    std::vector<std::string> args;
    if (kind == Kind::kVoiceIme && ::access(path.c_str(), X_OK) != 0) {
        usePath = true;  // python wrapper without +x
        args.push_back("python3");
        args.push_back(path);
    } else {
        args.push_back(path);
    }
    std::vector<char *> argv = ToArgvPointers(args);

    // envp
    std::vector<std::string> env;
    const std::string hostPidPrefix = "TYPELESS_A11Y_HOST_PID=";
    if (environ != nullptr) {
        for (char **e = environ; *e != nullptr; ++e) {
            if (kind == Kind::kAtspiHelper &&
                std::strncmp(*e, hostPidPrefix.c_str(), hostPidPrefix.size()) == 0)
                continue;
            env.emplace_back(*e);
        }
    }
    if (kind == Kind::kAtspiHelper)
        env.push_back(hostPidPrefix + std::to_string(::getpid()));
    if (kind == Kind::kAtspiHelper || kind == Kind::kVoiceIme) {
        const char *appDir = ::getenv("APPDIR");
        if (appDir != nullptr && appDir[0] != '\0')
            env = SanitizeAtspiHelperEnvironmentEntries(env, std::string(appDir));
    }
    std::vector<char *> envp = ToArgvPointers(env);

    // Deviation from the original (which passes attr=NULL): start the child
    // with an empty signal mask and default SIGTERM/SIGINT/SIGHUP/SIGPIPE
    // dispositions, so that PDEATHSIG and our SIGTERM work even when the
    // calling worker thread has signals blocked.
    posix_spawnattr_t attr;
    posix_spawnattr_t *attrPtr = nullptr;
    if (posix_spawnattr_init(&attr) == 0) {
        sigset_t empty, defaults;
        sigemptyset(&empty);
        sigemptyset(&defaults);
        sigaddset(&defaults, SIGTERM);
        sigaddset(&defaults, SIGINT);
        sigaddset(&defaults, SIGHUP);
        sigaddset(&defaults, SIGPIPE);
        posix_spawnattr_setsigmask(&attr, &empty);
        posix_spawnattr_setsigdefault(&attr, &defaults);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        attrPtr = &attr;
    }

    pid_t pid = -1;
    std::function<int()> job = [&]() {
        return usePath ? posix_spawnp(&pid, argv[0], actionsPtr, attrPtr, argv.data(), envp.data())
                       : posix_spawn(&pid, path.c_str(), actionsPtr, attrPtr, argv.data(),
                                     envp.data());
    };
    int rc = SpawnerThread::Get().Run(job);
    if (attrPtr)
        posix_spawnattr_destroy(attrPtr);
    if (actionsPtr)
        posix_spawn_file_actions_destroy(actionsPtr);
    if (logFd >= 0)
        ::close(logFd);
    if (nullFd >= 0)
        ::close(nullFd);
    if (rc != 0)
        return false;

    g_spawnedPid[idx] = pid;
    if (WaitUntilConnectableLocked(kind))
        return true;
    ReportSpawnFailure(kind);
    return false;
}

void TerminateKindImpl(Kind kind, bool stopRespawn)
{
    int idx = Index(kind);
    const char *name = Config(kind).binaryName;
    std::unique_lock<std::mutex> lock(g_mutex);
    if (stopRespawn)
        g_respawnStopped[idx] = true;

    int target = -1;
    bool isOurs = false;
    ReapSpawnedLocked(idx);
    int spawned = g_spawnedPid[idx];
    if (spawned > 0 && ::kill(spawned, 0) == 0 && !ProcessIsZombie(spawned) &&
        CmdlineContains(spawned, name)) {
        target = spawned;
        isOurs = true;
    } else {
        int filePid = ReadPidFile(kind);
        if (filePid > 0) {
            target = filePid;
            isOurs = (filePid == g_spawnedPid[idx]);
        }
    }

    if (target > 0 && ::kill(target, 0) == 0 && !ProcessIsZombie(target) &&
        CmdlineContains(target, name)) {
        if (isOurs)
            g_spawnedPid[idx] = -1;
        lock.unlock();
        SignalAndWait(target, isOurs);
        lock.lock();
    } else {
        g_spawnedPid[idx] = -1;
    }
    if (g_spawnedPid[idx] == target)
        g_spawnedPid[idx] = -1;
    std::string pidPath = PidFilePath(kind);
    if (!pidPath.empty())
        ::unlink(pidPath.c_str());
}

bool IsPathUnderDir(const std::string &path, const std::string &dir)
{
    if (dir.empty())
        return false;
    if (path.size() == dir.size())
        return path == dir;
    if (path.size() > dir.size())
        return path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/';
    return false;
}

}  // namespace

std::string RuntimeDirectory(Kind kind)
{
    char buf[512];
    if (TypelessNativeChildRuntimeDirectory(Config(kind).runtimeName, buf, sizeof buf) != 0)
        return std::string();
    return std::string(buf);
}

std::string SocketPath(Kind kind)
{
    return RuntimeDirectory(kind) + "/" + Config(kind).socketName;
}

std::string PidFilePath(Kind kind)
{
    return RuntimeDirectory(kind) + "/" + Config(kind).pidName;
}

std::string ResolvedBinaryPath(Kind kind)
{
    return ResolveBinaryPath(kind);
}

void StopRespawn(Kind kind)
{
    g_respawnStopped[Index(kind)] = true;
}

bool RespawnStopped(Kind kind)
{
    return g_respawnStopped[Index(kind)];
}

bool EnsureReady(Kind kind)
{
    int idx = Index(kind);
    const char *name = Config(kind).binaryName;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_respawnStopped[idx])
        return false;

    ReapSpawnedLocked(idx);
    int fd = TryConnect(kind);
    if (fd >= 0) {
        ::close(fd);
        return true;
    }

    // Something may still own the pid file (starting up, stuck, or an
    // orphan from a previous host process).
    int pid = ReadPidFile(kind);
    if (pid > 0) {
        if (::kill(pid, 0) == 0 && !ProcessIsZombie(pid) && CmdlineContains(pid, name)) {
            int ppid = ReadPidFileParent(kind);
            if (ppid <= 0)
                ppid = ReadParentPid(pid);
            bool parentAlive = false;
            if (ppid > 1) {
                if (ppid == ::getpid())
                    parentAlive = true;
                else if (::kill(ppid, 0) == 0 || errno != ESRCH)
                    parentAlive = true;
            }
            if (parentAlive) {
                // Another live host owns a helper that is starting: wait for it.
                if (WaitUntilConnectableLocked(kind))
                    return true;
            } else {
                // Orphaned helper: kill it and spawn a fresh one.
                SignalAndWait(pid, g_spawnedPid[idx] == pid);
                g_spawnedPid[idx] = -1;
            }
        } else {
            int status = 0;
            ::waitpid(pid, &status, WNOHANG);
        }
    }

    auto now = std::chrono::steady_clock::now();
    if (g_nextSpawnAtSet[idx] && g_nextSpawnAt[idx] > now) {
        fd = TryConnect(kind);
        if (fd >= 0) {
            ::close(fd);
            return true;
        }
        return false;
    }
    g_nextSpawnAt[idx] = now + kSpawnThrottle;
    g_nextSpawnAtSet[idx] = true;
    if (SpawnLocked(kind))
        return true;
    fd = TryConnect(kind);
    if (fd >= 0) {
        ::close(fd);
        return true;
    }
    return false;
}

int Connect(Kind kind)
{
    if (!EnsureReady(kind))
        return -1;
    return TryConnect(kind);
}

void Terminate(Kind kind)
{
    TerminateKindImpl(kind, true);
}

void KillForRestart(Kind kind)
{
    TerminateKindImpl(kind, false);
}

int ReadParentPid(int pid)
{
    if (pid <= 0)
        return -1;
    std::ifstream in("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 5, "PPid:") == 0) {
            std::istringstream ss(line.substr(5));
            int ppid = -1;
            if (ss >> ppid)
                return ppid;
            return -1;
        }
    }
    return -1;
}

bool ProcessIsZombie(int pid)
{
    if (pid <= 0)
        return false;
    std::ifstream in("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 6, "State:") == 0)
            return line.find('Z') != std::string::npos;
    }
    return false;
}

bool CmdlineContains(int pid, const char *needle)
{
    if (pid <= 0 || needle == nullptr)
        return false;
    std::ifstream in("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str().find(needle) != std::string::npos;
}

bool ProcessIsLiveHelper(int pid, const char *binaryName)
{
    if (pid <= 0)
        return false;
    if (::kill(pid, 0) != 0)
        return false;
    if (ProcessIsZombie(pid))
        return false;
    return CmdlineContains(pid, binaryName);
}

std::vector<std::string> SanitizeAtspiHelperEnvironmentEntries(
    const std::vector<std::string> &entries, const std::string &appDir)
{
    if (appDir.empty())
        return entries;
    std::vector<std::string> out;
    out.reserve(entries.size());
    for (const auto &entry : entries) {
        size_t eq = entry.find('=');
        if (eq == std::string::npos)
            continue;  // malformed entries are dropped
        std::string key = entry.substr(0, eq);
        std::string value = entry.substr(eq + 1);
        if (key != "LD_LIBRARY_PATH" && key != "GTK_PATH" && key != "GIO_MODULE_DIR") {
            out.push_back(entry);
            continue;
        }
        std::string kept;
        size_t start = 0;
        while (start <= value.size()) {
            size_t colon = value.find(':', start);
            std::string part = value.substr(start, colon == std::string::npos ? std::string::npos
                                                                             : colon - start);
            if (!part.empty() && !IsPathUnderDir(part, appDir)) {
                if (!kept.empty())
                    kept.push_back(':');
                kept += part;
            }
            if (colon == std::string::npos)
                break;
            start = colon + 1;
        }
        if (!kept.empty())
            out.push_back(key + "=" + kept);
    }
    return out;
}

}  // namespace NativeChild
}  // namespace LinuxShared
