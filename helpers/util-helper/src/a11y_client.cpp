// Minimal client for the shared typeless-atspi-helper child process
// (LinuxShared::NativeChild + LinuxShared::A11y::Client subset used by
// libUtilHelper: EnsureReady / IsDesktopReachable / Disconnect).
//
// Wire protocol: unix stream socket $XDG_RUNTIME_DIR/typeless-atspi/helper.sock
// (fallback /tmp/typeless-atspi-<uid>/helper.sock); frames are a 4-byte
// little-endian length (1..8 MiB) followed by a JSON object.
// Request:  {"id":N,"cmd":"IsDesktopReachable","timeoutMs":300,...}
// Response: {"id":N,"ok":true,...,"payload":{"reachable":true}}
// The helper writes "<pid>\n<parent pid>\n" to helper.pid in the same dir and
// is spawned with TYPELESS_A11Y_HOST_PID=<our pid>, stdout=/dev/null,
// stderr=<dir>/helper.stderr.log.
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#include "common.h"

extern char** environ;

namespace {

constexpr const char kHelperBinaryName[] = "typeless-atspi-helper";
constexpr const char kRuntimeDirName[] = "typeless-atspi";
constexpr uint32_t kMaxFrameBytes = 0x800000;

// ---------------------------------------------------------------- NativeChild
std::mutex g_childMutex;
pid_t g_childPid = -1;
std::chrono::steady_clock::time_point g_nextSpawnAllowed{};

std::string RuntimeDirectory() {
  const char* xdg = getenv("XDG_RUNTIME_DIR");
  char buf[512];
  if (xdg && *xdg) {
    snprintf(buf, sizeof(buf), "%s/%s", xdg, kRuntimeDirName);
  } else {
    snprintf(buf, sizeof(buf), "/tmp/%s-%d", kRuntimeDirName, (int)getuid());
  }
  return buf;
}
std::string SocketPath() { return RuntimeDirectory() + "/helper.sock"; }
std::string PidFilePath() { return RuntimeDirectory() + "/helper.pid"; }

int TryConnect() {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::string path = SocketPath();
  if (path.size() >= sizeof(addr.sun_path)) {
    close(fd);
    return -1;
  }
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  timeval tv{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  return fd;
}

void ReapChild() {
  if (g_childPid <= 0) return;
  int status = 0;
  pid_t r = waitpid(g_childPid, &status, WNOHANG);
  if (r == g_childPid || (r < 0 && errno == ECHILD)) g_childPid = -1;
}

bool WaitUntilConnectable() {
  for (int i = 0; i < 40; ++i) {
    int fd = TryConnect();
    if (fd >= 0) {
      close(fd);
      return true;
    }
    ReapChild();
    poll(nullptr, 0, 25);
  }
  return false;
}

void ReadPidFile(pid_t* pid, pid_t* parent) {
  *pid = -1;
  *parent = -1;
  std::ifstream f(PidFilePath());
  if (!f.is_open()) return;
  long a = -1, b = -1;
  if (f >> a) *pid = (pid_t)a;
  if (f >> b) *parent = (pid_t)b;
}

std::string ReadFileString(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return std::string();
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool ProcessIsZombie(pid_t pid) {
  std::string st = ReadFileString("/proc/" + std::to_string(pid) + "/status");
  size_t p = st.find("State:");
  if (p == std::string::npos) return false;
  p = st.find_first_not_of(" \t", p + 6);
  return p != std::string::npos && (st[p] == 'Z' || st[p] == 'X');
}

pid_t ReadParentPid(pid_t pid) {
  std::string st = ReadFileString("/proc/" + std::to_string(pid) + "/status");
  size_t p = st.find("PPid:");
  if (p == std::string::npos) return -1;
  return (pid_t)strtol(st.c_str() + p + 5, nullptr, 10);
}

bool CmdlineContains(pid_t pid, const char* needle) {
  std::string c = ReadFileString("/proc/" + std::to_string(pid) + "/cmdline");
  for (char& ch : c)
    if (ch == '\0') ch = ' ';
  return c.find(needle) != std::string::npos;
}

void SignalAndWait(pid_t pid) {
  if (pid <= 1 || pid == getpid()) return;
  kill(pid, SIGTERM);
  for (int i = 0; i < 40; ++i) {
    if (pid == g_childPid) ReapChild();
    if (kill(pid, 0) != 0 || ProcessIsZombie(pid)) return;
    poll(nullptr, 0, 25);
  }
  kill(pid, SIGKILL);
  if (pid == g_childPid) {
    int st;
    waitpid(pid, &st, 0);
    g_childPid = -1;
  }
}

bool IsExecutableFile(const std::string& p) {
  struct stat st;
  return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0;
}

std::string DirectoryOfPath(const std::string& p) {
  size_t s = p.rfind('/');
  if (s == std::string::npos) return std::string();
  return s == 0 ? std::string("/") : p.substr(0, s);
}

std::string ResolveBinaryPath() {
  const char* env = getenv("TYPELESS_ATSPI_HELPER");
  if (env && *env) return env;
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(&ResolveBinaryPath), &info) && info.dli_fname) {
    std::string cand = DirectoryOfPath(info.dli_fname) + "/" + kHelperBinaryName;
    if (IsExecutableFile(cand)) return cand;
  }
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n > 0) {
    exe[n] = '\0';
    std::string dir = DirectoryOfPath(exe);
    for (const char* rel : {"/", "/resources/lib/util-helper/build/linux/arm64/",
                            "/resources/lib/linux-shared/build/linux/arm64/"}) {
      std::string cand = dir + rel + kHelperBinaryName;
      if (IsExecutableFile(cand)) return cand;
    }
  }
  return std::string();
}

// Under an AppImage, drop APPDIR-internal entries of LD_LIBRARY_PATH /
// GIO_MODULE_DIR so the helper uses host GLib/AT-SPI.
std::vector<std::string> BuildChildEnvironment() {
  std::vector<std::string> env;
  const char* appdir = getenv("APPDIR");
  std::string ad = (appdir && *appdir) ? appdir : "";
  for (char** e = environ; e && *e; ++e) {
    std::string entry(*e);
    if (entry.rfind("TYPELESS_A11Y_HOST_PID=", 0) == 0) continue;
    if (!ad.empty() && (entry.rfind("LD_LIBRARY_PATH=", 0) == 0 || entry.rfind("GIO_MODULE_DIR=", 0) == 0)) {
      size_t eq = entry.find('=');
      std::string key = entry.substr(0, eq + 1), val = entry.substr(eq + 1), kept;
      std::stringstream ss(val);
      std::string part;
      while (std::getline(ss, part, ':')) {
        if (part.empty() || part.rfind(ad, 0) == 0) continue;
        if (!kept.empty()) kept += ':';
        kept += part;
      }
      if (kept.empty()) continue;
      entry = key + kept;
    }
    env.push_back(entry);
  }
  env.push_back("TYPELESS_A11Y_HOST_PID=" + std::to_string(getpid()));
  return env;
}

bool SpawnLocked() {
  std::string bin = ResolveBinaryPath();
  if (bin.empty()) {
    fprintf(stderr, "native_child: %s not found\n", kHelperBinaryName);
    return false;
  }
  std::string dir = RuntimeDirectory();
  if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
  chmod(dir.c_str(), 0700);
  std::string logPath = dir + "/helper.stderr.log";

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  int logFd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (logFd >= 0) {
    posix_spawn_file_actions_adddup2(&fa, logFd, 2);
  }
  int nullFd = open("/dev/null", O_RDWR | O_CLOEXEC);
  if (nullFd >= 0) {
    posix_spawn_file_actions_adddup2(&fa, nullFd, 1);
  }
  std::vector<std::string> env = BuildChildEnvironment();
  std::vector<char*> envp;
  for (auto& s : env) envp.push_back(const_cast<char*>(s.c_str()));
  envp.push_back(nullptr);
  std::vector<char*> argv{const_cast<char*>(bin.c_str()), nullptr};

  pid_t pid = -1;
  int rc = posix_spawn(&pid, bin.c_str(), &fa, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&fa);
  if (logFd >= 0) close(logFd);
  if (nullFd >= 0) close(nullFd);
  if (rc != 0) return false;
  g_childPid = pid;
  if (WaitUntilConnectable()) return true;

  std::string log = ReadFileString(logPath);
  if (!log.empty()) {
    fprintf(stderr, "native_child: %s stderr: %s\n", kHelperBinaryName, log.c_str());
    if (log.find("undefined symbol") != std::string::npos || log.find("GLIBC_") != std::string::npos) {
      fprintf(stderr, "native_child: %s ABI mismatch.\n", kHelperBinaryName);
    }
  }
  return false;
}

bool NativeChildEnsureReady() {
  std::lock_guard<std::mutex> lk(g_childMutex);
  ReapChild();
  int fd = TryConnect();
  if (fd >= 0) {
    close(fd);
    return true;
  }
  pid_t pid, parent;
  ReadPidFile(&pid, &parent);
  if (pid > 0 && pid != getpid() && kill(pid, 0) == 0) {
    if (!ProcessIsZombie(pid) && CmdlineContains(pid, kHelperBinaryName)) {
      if (parent <= 0) parent = ReadParentPid(pid);
      bool ownerAlive = parent > 1 && (parent == getpid() || kill(parent, 0) == 0 || errno != ESRCH);
      if (ownerAlive) {
        if (WaitUntilConnectable()) return true;
      } else {
        SignalAndWait(pid);  // orphaned helper
      }
    }
  }
  auto now = std::chrono::steady_clock::now();
  if (g_nextSpawnAllowed.time_since_epoch().count() != 0 && now < g_nextSpawnAllowed) {
    fd = TryConnect();
    if (fd >= 0) close(fd);
    return fd >= 0;
  }
  if (SpawnLocked()) return true;
  g_nextSpawnAllowed = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  return false;
}

int NativeChildConnect() {
  if (!NativeChildEnsureReady()) return -1;
  return TryConnect();
}

// -------------------------------------------------------------- A11y client
std::mutex g_clientMutex;
int g_fd = -1;
uint64_t g_nextId = 1;

bool WriteAll(int fd, const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  while (len > 0) {
    ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    p += n;
    len -= (size_t)n;
  }
  return true;
}

bool WriteFrame(int fd, const std::string& payload) {
  if (fd < 0 || payload.size() > kMaxFrameBytes) return false;
  uint32_t n = (uint32_t)payload.size();
  unsigned char hdr[4] = {(unsigned char)n, (unsigned char)(n >> 8), (unsigned char)(n >> 16), 0};
  return WriteAll(fd, hdr, 4) && WriteAll(fd, payload.data(), payload.size());
}

bool ReadAll(int fd, void* data, size_t len, int timeoutMs) {
  char* p = static_cast<char*>(data);
  while (len > 0) {
    pollfd pfd{fd, POLLIN, 0};
    int r = poll(&pfd, 1, timeoutMs);
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) return false;
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) && !(pfd.revents & POLLIN)) return false;
    ssize_t n = recv(fd, p, len, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    p += n;
    len -= (size_t)n;
  }
  return true;
}

bool ReadFrame(int fd, std::string* out, int timeoutMs) {
  out->clear();
  unsigned char hdr[4];
  if (!ReadAll(fd, hdr, 4, timeoutMs)) return false;
  uint32_t n = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
  if (n == 0 || n > kMaxFrameBytes) return false;
  out->resize(n);
  return ReadAll(fd, &(*out)[0], n, timeoutMs);
}

std::string EncodeRequest(uint64_t id, const std::string& cmd, int timeoutMs) {
  // Same key order/defaults as LinuxShared::A11y::EncodeRequest.
  std::ostringstream os;
  os << "{\"id\":" << id << ",\"cmd\":\"" << cmd << "\",\"timeoutMs\":" << timeoutMs
     << ",\"processId\":0,\"beforeChars\":0,\"afterChars\":0,\"maxLength\":0,\"reassert\":false"
     << ",\"windowTitle\":\"\",\"busName\":\"\",\"objectPath\":\"\",\"generation\":0,\"foregroundNote\":\"\""
     << ",\"walkMaxDepth\":0,\"walkMaxBreadth\":0,\"walkMaxNodes\":0,\"parentWalkMaxLevels\":0"
     << ",\"editableMaxAncestorLevels\":0,\"editableMaxDescendantNodes\":0,\"editableMaxBreadth\":0"
     << ",\"editableMaxInputTextLength\":0,\"textMaxCharsPerCall\":0,\"focusScanTimeoutMs\":0}";
  return os.str();
}

// Finds `"key":` and parses a JSON boolean after it.
bool JsonReadBool(const std::string& json, const char* key, bool* out) {
  std::string pat = std::string("\"") + key + "\":";
  size_t p = json.find(pat);
  if (p == std::string::npos) return false;
  p = json.find_first_not_of(" \t\r\n", p + pat.size());
  if (p == std::string::npos) return false;
  if (json.compare(p, 4, "true") == 0) {
    *out = true;
    return true;
  }
  if (json.compare(p, 5, "false") == 0) {
    *out = false;
    return true;
  }
  return false;
}

std::string ExtractPayload(const std::string& json) {
  const std::string pat = "\"payload\":";
  size_t p = json.find(pat);
  if (p == std::string::npos) return "{}";
  p = json.find_first_not_of(" \t\r\n", p + pat.size());
  if (p == std::string::npos || json[p] != '{') return "{}";
  int depth = 0;
  bool inStr = false;
  for (size_t i = p; i < json.size(); ++i) {
    char c = json[i];
    if (inStr) {
      if (c == '\\') ++i;
      else if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') inStr = true;
    else if (c == '{') ++depth;
    else if (c == '}' && --depth == 0) return json.substr(p, i - p + 1);
  }
  return "{}";
}

void DisconnectLocked() {
  if (g_fd >= 0) {
    shutdown(g_fd, SHUT_RDWR);
    close(g_fd);
    g_fd = -1;
  }
}

bool EnsureConnectedLocked() {
  if (g_fd >= 0) return true;
  g_fd = NativeChildConnect();
  return g_fd >= 0;
}

struct CallResult {
  bool ok = false;
  std::string failReason;
  std::string payload = "{}";
};

CallResult CallOnceLocked(uint64_t id, const std::string& cmd, int timeoutMs) {
  CallResult r;
  if (!EnsureConnectedLocked()) {
    r.failReason = "helper_unavailable";
    return r;
  }
  if (!WriteFrame(g_fd, EncodeRequest(id, cmd, timeoutMs))) {
    DisconnectLocked();
    r.failReason = "write_failed";
    return r;
  }
  std::string resp;
  if (!ReadFrame(g_fd, &resp, timeoutMs + 200)) {
    DisconnectLocked();
    r.failReason = "read_timeout";
    return r;
  }
  bool ok = false;
  if (resp.empty() || resp[0] != '{' || !JsonReadBool(resp, "ok", &ok)) {
    DisconnectLocked();
    r.failReason = "decode_failed";
    return r;
  }
  r.ok = ok;
  r.payload = ExtractPayload(resp);
  return r;
}

CallResult Call(const std::string& cmd, int timeoutMs) {
  std::lock_guard<std::mutex> lk(g_clientMutex);
  if (timeoutMs <= 0) timeoutMs = 300;
  CallResult r = CallOnceLocked(g_nextId++, cmd, timeoutMs);
  if (!r.ok && (r.failReason == "helper_unavailable" || r.failReason == "write_failed")) {
    DisconnectLocked();
    r = CallOnceLocked(g_nextId++, cmd, timeoutMs);
  }
  return r;
}

}  // namespace

namespace a11y_client {

bool EnsureReady() {
  std::lock_guard<std::mutex> lk(g_clientMutex);
  bool ok = EnsureConnectedLocked();
  if (!ok) fprintf(stderr, "[util-helper] EnsureReady: cannot connect to or start typeless-atspi-helper\n");
  return ok;
}

// 1 = reachable, 0 = helper says unreachable, -1 = no answer (helper busy
// with another client's request, timed out, or unavailable).
int IsDesktopReachable() {
  CallResult r = Call("IsDesktopReachable", 300);
  bool reachable = false;
  if (!r.ok || !JsonReadBool(r.payload, "reachable", &reachable)) {
    fprintf(stderr, "[util-helper] IsDesktopReachable: no answer (%s) payload=%s\n",
            r.failReason.empty() ? "not ok" : r.failReason.c_str(), r.payload.c_str());
    return -1;
  }
  if (!reachable) fprintf(stderr, "[util-helper] IsDesktopReachable: helper says unreachable %s\n", r.payload.c_str());
  return reachable ? 1 : 0;
}

void Disconnect() {
  std::lock_guard<std::mutex> lk(g_clientMutex);
  DisconnectLocked();
}

}  // namespace a11y_client
