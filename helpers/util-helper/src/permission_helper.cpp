#include <atomic>
#include <mutex>

#include <gio/gio.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include "common.h"

namespace {

bool CanOpen(const char* path, int flags) {
  int fd = open(path, flags | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return false;
  close(fd);
  return true;
}

// 1 if the process has the "input" group (effective, real or supplementary).
int ProcessHasInputGroup() {
  std::vector<char> buf(16384, 0);
  struct group gr;
  struct group* res = nullptr;
  int rc = getgrnam_r("input", &gr, buf.data(), buf.size(), &res);
  if (rc != 0) return rc;
  if (!res) return 0;
  gid_t gid = res->gr_gid;
  if (getegid() == gid || getgid() == gid) return 1;
  int n = getgroups(0, nullptr);
  if (n <= 0) return 0;
  std::vector<gid_t> groups((size_t)n, 0);
  n = getgroups(n, groups.data());
  if (n < 0) return 0;
  for (int i = 0; i < n; ++i)
    if (groups[(size_t)i] == gid) return 1;
  return 0;
}

}  // namespace

// Hotkeys/text injection need write access to /dev/uinput and read access to
// /dev/input/event* (or membership of the input group).
UH_EXPORT int checkInputDevicesPermission() {
  if (!CanOpen("/dev/uinput", O_WRONLY)) return 0;
  if (DIR* d = opendir("/dev/input")) {
    while (dirent* ent = readdir(d)) {
      if (strncmp(ent->d_name, "event", 5) != 0) continue;
      std::string path = std::string("/dev/input/") + ent->d_name;
      if (CanOpen(path.c_str(), O_RDONLY)) {
        closedir(d);
        return 1;
      }
    }
    closedir(d);
  }
  return ProcessHasInputGroup();
}

// "Accessibility permission" on Linux = the AT-SPI desktop is reachable via
// the typeless-atspi-helper child process.
// The app polls this every 2 s on its main thread and raises an alert on a
// single 0. Asking typeless-atspi-helper here (as the original does) can
// deadlock: while a Typeless window has focus the helper makes AT-SPI calls
// into Typeless itself, whose main thread is blocked in this very call, so
// every check times out. "Is the desktop accessibility bus reachable" is
// answered by the bus launcher directly, without involving our own process.
static std::atomic<int> g_lastAccessibility{0};
static std::mutex g_a11yBusMutex;

// 1 = reachable, 0 = no accessibility bus, -1 = could not tell (transient).
static int QueryAccessibilityBus() {
  std::lock_guard<std::mutex> lk(g_a11yBusMutex);
  GError* err = nullptr;
  GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
  if (!conn) {
    g_clear_error(&err);
    return -1;
  }
  GVariant* reply = g_dbus_connection_call_sync(
      conn, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress", nullptr,
      G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 500, nullptr, &err);
  g_object_unref(conn);
  if (!reply) {
    bool absent = g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
                  g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER);
    g_clear_error(&err);
    return absent ? 0 : -1;
  }
  const char* address = nullptr;
  g_variant_get(reply, "(&s)", &address);
  int ok = (address && *address) ? 1 : 0;
  g_variant_unref(reply);
  return ok;
}

UH_EXPORT int checkAccessibilityPermission() {
  int reachable = QueryAccessibilityBus();
  if (reachable < 0) return g_lastAccessibility.load();
  g_lastAccessibility.store(reachable);
  return reachable;
}
