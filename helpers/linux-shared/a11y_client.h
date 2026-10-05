// a11y_client.h - in-process client for typeless-atspi-helper (interoperable
// reimplementation of Typeless' LinuxShared::A11y::Client).
//
// Thread-safe: every call takes one process-wide (per shared object) mutex,
// so calls are serialized, exactly like the original. Blocking bound per
// Call(): helper (re)spawn wait (only when the helper is not running; at
// most ~8 s, normally < 100 ms) + write + read of (timeoutMs + 200 ms),
// and at most one automatic retry after helper_unavailable / write_failed /
// read_timeout (see SetRetryAfterReadTimeout).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "a11y_ipc.h"
#include "a11y_types.h"

namespace LinuxShared {
namespace A11y {

class Client {
public:
    static Client &Instance();

    // Connects (spawning the helper if necessary). false when the helper is
    // unavailable or respawn has been stopped.
    bool EnsureReady();
    // Drops the socket connection (the helper keeps running).
    void Disconnect();
    // Stops respawning and shuts the connection down (helper keeps running
    // until parent death / Terminate).
    void BeginShutdown();
    // BeginShutdown() + NativeChild::Terminate(kAtspiHelper).
    void TerminateHelper();

    // Generic call. request.id is assigned here; timeoutMs <= 0 becomes 300.
    // Transport failures come back as ok=false with error kDisabled
    // (failReason "helper_unavailable" / "write_failed" / "decode_failed") or
    // kTimeout ("read_timeout"), payload "{}".
    A11yResponse Call(A11yRequest request);

    bool EnableSessionA11yStatus(bool reassert);                // payload {"advertised":bool}; returns ok
    void NoteForegroundProcessId(int processId);
    void NoteOwnProcessForeground();
    void ReleaseCompositorForegroundLock();
    A11yResponse GetActiveWindowSnapshot(int timeoutMs, int processId, ForegroundNote note);
    A11yResponse GetActiveWindowSnapshot(int timeoutMs);
    A11yResponse GetFocusSnapshot(int timeoutMs);
    A11yResponse GetSurroundingText(int beforeChars, int afterChars, int timeoutMs,
                                    const FocusKey &focusKey);
    A11yResponse GetVisibleText(int maxLength, int timeoutMs);
    A11yResponse FindDocumentUrlInActiveWindow(const std::string &windowTitle, int processId,
                                               int timeoutMs);
    // payload {"valid":bool,"error":"...","beforeText":"...","selectedText":"...","afterText":"..."}
    A11yResponse ReadFocusedInputState(int timeoutMs);
    Atspi::FocusedInputCapability ProbeFocusedInputCapability();
    bool IsDesktopReachable();
    bool RecoverAfterResume();

    // Extension (not in the original API): when false, a read_timeout is not
    // retried, so a Call() never blocks longer than one round trip.
    // Default true (= original behaviour).
    static void SetRetryAfterReadTimeout(bool enabled);

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

private:
    Client() = default;
    bool EnsureConnectedLocked();
    void DisconnectLocked();
    A11yResponse CallOnceLocked(A11yRequest &request);

    std::atomic<int> socketFd_{-1};
    uint64_t nextId_ = 1;
};

}  // namespace A11y
}  // namespace LinuxShared
