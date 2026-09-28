#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>

#include "common.h"

namespace {

std::string ToLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
  return s;
}

std::string FileNameFromPath(const std::string& path) {
  size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) return path;
  return path.substr(pos + 1);
}

// True when both refer to the same executable: identical inode, or (fallback)
// the same case-insensitive file name.
bool SameExecutableIdentity(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return false;
  struct stat sa {}, sb {};
  if (stat(a.c_str(), &sa) == 0 && stat(b.c_str(), &sb) == 0) {
    if (sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino) return true;
  }
  if (a == b) return true;
  std::string fa = ToLowerAscii(FileNameFromPath(a));
  std::string fb = ToLowerAscii(FileNameFromPath(b));
  return !fa.empty() && fa == fb;
}

}  // namespace

std::string ResolveProcessExecutablePath(uint32_t pid) {
  if (pid == 0) return std::string();
  char link[64];
  snprintf(link, sizeof(link), "/proc/%u/exe", pid);
  char buf[4096];
  ssize_t n = readlink(link, buf, sizeof(buf) - 1);
  if (n <= 0) return std::string();
  buf[n] = '\0';
  return std::string(buf);
}

bool ShouldSkipSinkInputForDuck(const SinkInputDuckIdentity& id, const SinkInputDuckSelf& self) {
  // Corked (paused) streams are left alone.
  if (id.corked) return true;
  // Our own process.
  if (id.processId != 0 && id.processId == self.processId) return true;
  // Event sounds (notifications etc.).
  if (ToLowerAscii(id.mediaRole) == "event") return true;
  // Another process of the same executable (e.g. Electron's audio service).
  if (SameExecutableIdentity(self.executablePath, id.processBinary)) return true;
  if (id.processId != 0 && id.processId != self.processId) {
    return SameExecutableIdentity(self.executablePath, ResolveProcessExecutablePath(id.processId));
  }
  return false;
}
