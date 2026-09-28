// deviceIsLidOpen(): 1 = open, 0 = closed, -1 = unknown / no lid.
#include <dirent.h>
#include <string.h>

#include <fstream>

#include "common.h"

namespace {

constexpr const char kLidNodeNameFragment[] = "lid";
constexpr const char kInputClassDevicePrefix[] = "input";

bool IsDotEntry(const char* n) { return n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')); }

bool ReadTrimmedLine(const std::string& path, std::string* out) {
  std::ifstream f(path);
  if (!f.is_open()) return false;
  std::string line;
  if (!std::getline(f, line)) return false;
  size_t b = line.find_first_not_of(" \t\r\n");
  size_t e = line.find_last_not_of(" \t\r\n");
  *out = (b == std::string::npos) ? std::string() : line.substr(b, e - b + 1);
  return true;
}

// /proc/acpi/button/lid/*/state: "state:      open" / "state:      closed".
int ReadAcpiLidState() {
  DIR* d = opendir("/proc/acpi/button/lid");
  if (!d) return -1;
  int result = -1;
  while (dirent* ent = readdir(d)) {
    if (IsDotEntry(ent->d_name)) continue;
    std::string value;
    if (!ReadTrimmedLine(std::string("/proc/acpi/button/lid/") + ent->d_name + "/state", &value)) continue;
    if (value.find("open") != std::string::npos) {
      result = 1;
      break;
    }
    if (value.find("closed") != std::string::npos) {
      result = 0;
      break;
    }
  }
  closedir(d);
  return result;
}

// /sys/class/input/input*/<*lid*>/state: "0" = open, "1" = closed.
int ReadSysfsLidState() {
  DIR* d = opendir("/sys/class/input");
  if (!d) return -1;
  int result = -1;
  while (result == -1) {
    dirent* ent = readdir(d);
    if (!ent) break;
    if (IsDotEntry(ent->d_name)) continue;
    if (strncmp(ent->d_name, kInputClassDevicePrefix, 5) != 0) continue;
    std::string dir = std::string("/sys/class/input/") + ent->d_name;
    DIR* sub = opendir(dir.c_str());
    if (!sub) continue;
    while (dirent* s = readdir(sub)) {
      if (IsDotEntry(s->d_name)) continue;
      if (!strstr(s->d_name, kLidNodeNameFragment)) continue;
      std::string value;
      if (!ReadTrimmedLine(dir + "/" + s->d_name + "/state", &value) || value.size() != 1) continue;
      if (value[0] == '1') {
        result = 0;
        break;
      }
      if (value[0] == '0') {
        result = 1;
        break;
      }
    }
    closedir(sub);
  }
  closedir(d);
  return result;
}

}  // namespace

UH_EXPORT int32_t deviceIsLidOpen() {
  try {
    int r = ReadAcpiLidState();
    if (r != -1) return r;
    r = ReadSysfsLidState();
    if (r != -1) return r;
  } catch (...) {
  }
  return -1;
}
