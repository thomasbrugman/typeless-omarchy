#include <sys/wait.h>
#include <unistd.h>

#include <thread>

#include "common.h"

typedef void (*AudioMonitorCallback)(char*);

UH_EXPORT const char* util_say_hello() { return "Hello from UtilHelper Linux shared library!"; }

// Opens a URL / file / app name via xdg-open; 1 if xdg-open exited with 0.
UH_EXPORT int launchApplicationByName(const char* name) {
  if (!name || !*name) return 0;
  pid_t pid = fork();
  if (pid < 0) return 0;
  if (pid == 0) {
    execlp("xdg-open", "xdg-open", name, (char*)nullptr);
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return 0;
  return (status & 0xff7f) == 0 ? 1 : 0;
}

// Windows-only in the original; on Linux it asynchronously reports an error.
UH_EXPORT void queryWMIAsync(const char* /*className*/, const char* /*properties*/, AudioMonitorCallback cb) {
  if (!cb) return;
  try {
    std::thread([cb] {
      static char msg[] = "{\"error\": \"WMI query is only available on Windows\"}";
      cb(msg);
    }).detach();
  } catch (...) {
  }
}
