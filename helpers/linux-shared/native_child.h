// native_child.h - spawn / connect / terminate management for Typeless'
// native child processes (clean-room reimplementation of the x64
// LinuxShared::NativeChild namespace that lives inside libInputHelper.so,
// libContextHelper.so and libUtilHelper.so).
//
// Every shared library that links this file gets its own private copy of
// the state (compile with -fvisibility=hidden).
#pragma once

#include <string>
#include <vector>

namespace LinuxShared {
namespace NativeChild {

enum class Kind : int {
    kAtspiHelper = 0,  // typeless-atspi-helper   runtime "typeless-atspi"     helper.sock / helper.pid / helper.stderr.log
    kIbusEngine = 1,   // typeless-ibus-engine    runtime "typeless-ibus"      inject.sock / engine.pid / engine.stderr.log
    kVoiceIme = 2,     // typeless-voice-ime      runtime "typeless-voice-ime" inject.sock / engine.pid / engine.stderr.log
};

// "$XDG_RUNTIME_DIR/<runtime>" or "/tmp/<runtime>-<uid>" ("" on error).
std::string RuntimeDirectory(Kind kind);
std::string SocketPath(Kind kind);
std::string PidFilePath(Kind kind);

// Resolution order (first hit wins):
//   1. $TYPELESS_ATSPI_HELPER / $TYPELESS_IBUS_ENGINE / $TYPELESS_VOICE_IME
//      (returned verbatim when non-empty, not checked)
//   2. <dir of the .so/.exe containing this code (dladdr)>/<binary>
//   3. <parent of that dir>/<binary>
//   4. <dir of /proc/self/exe>/<binary>
// Candidates 2-4 must pass access(X_OK) (access(F_OK) for kVoiceIme).
// Returns "" when nothing is found.
std::string ResolvedBinaryPath(Kind kind);

// Makes sure a helper of this kind is running and accepting connections,
// spawning it if needed (throttled to one spawn attempt per 400 ms, waits
// up to 8 s (atspi) / 1 s (others) for the socket). Thread-safe.
bool EnsureReady(Kind kind);

// EnsureReady() + connect. Returns a connected AF_UNIX stream fd
// (SOCK_CLOEXEC, SO_SNDTIMEO 1 s) or -1.
int Connect(Kind kind);

// Stop respawning (sticky) and kill the helper (SIGTERM, 1.5 s grace,
// SIGKILL), unlink its pid file.
void Terminate(Kind kind);
// Same kill sequence but keeps respawn enabled.
void KillForRestart(Kind kind);
void StopRespawn(Kind kind);
bool RespawnStopped(Kind kind);

// /proc helpers.
int ReadParentPid(int pid);
bool ProcessIsZombie(int pid);
bool CmdlineContains(int pid, const char *needle);
bool ProcessIsLiveHelper(int pid, const char *binaryName);

// Removes $APPDIR-relative entries from LD_LIBRARY_PATH / GTK_PATH /
// GIO_MODULE_DIR (drops the variable when nothing is left) and drops
// entries without '='. Identity when appDir is empty.
std::vector<std::string> SanitizeAtspiHelperEnvironmentEntries(
    const std::vector<std::string> &entries, const std::string &appDir);

}  // namespace NativeChild
}  // namespace LinuxShared
