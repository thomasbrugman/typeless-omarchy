#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "common.h"

static std::string XdgDir(const char* var, const char* homeSuffix) {
  const char* v = getenv(var);
  if (v && *v) return v;
  const char* home = getenv("HOME");
  if (!home || !*home) return std::string();
  return std::string(home) + homeSuffix;
}

std::string GetXdgConfigHome() { return XdgDir("XDG_CONFIG_HOME", "/.config"); }
std::string GetXdgDataHome() { return XdgDir("XDG_DATA_HOME", "/.local/share"); }

bool EnsureDirectoryExists(const std::string& path) {
  if (path.empty()) return false;
  struct stat st;
  if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  std::string cur;
  cur.reserve(path.size());
  for (size_t i = 0; i < path.size(); ++i) {
    char c = path[i];
    cur.push_back(c);
    if ((c == '/' && i != 0) || i + 1 == path.size()) {
      std::string p = cur;
      if (p.size() > 1 && p.back() == '/') p.pop_back();
      if (stat(p.c_str(), &st) == 0) {
        if (!S_ISDIR(st.st_mode)) return false;
        continue;
      }
      if (mkdir(p.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
  }
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
