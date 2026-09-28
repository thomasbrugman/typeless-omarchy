// a11y_client.cpp - clean-room reimplementation of LinuxShared::A11y::Client
// (x64 libInputHelper.so / libContextHelper.so / libUtilHelper.so).
#include "a11y_client.h"

#include <mutex>

#include <sys/socket.h>
#include <unistd.h>

#include "native_child.h"

namespace LinuxShared {
namespace A11y {
namespace {

std::mutex g_clientMutex;
std::atomic<bool> g_retryAfterReadTimeout{true};

constexpr int kDefaultTimeoutMs = 300;
constexpr int kReadSlackMs = 200;

A11yResponse TransportFailure(A11yError error, const char *reason)
{
    A11yResponse r;
    r.id = 0;
    r.ok = false;
    r.error = error;
    r.failReason = reason;
    r.generation = 0;
    r.payloadJson = "{}";
    return r;
}

A11yRequest MakeRequest(const char *command)
{
    A11yRequest r;
    r.command = command;
    return r;  // timeoutMs defaults to 300
}

}  // namespace

Client &Client::Instance()
{
    static Client instance;
    return instance;
}

void Client::DisconnectLocked()
{
    int fd = socketFd_.exchange(-1);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
}

bool Client::EnsureConnectedLocked()
{
    if (NativeChild::RespawnStopped(NativeChild::Kind::kAtspiHelper)) {
        DisconnectLocked();
        return false;
    }
    if (socketFd_.load() >= 0)
        return true;
    int fd = NativeChild::Connect(NativeChild::Kind::kAtspiHelper);
    socketFd_.store(fd);
    return fd >= 0;
}

A11yResponse Client::CallOnceLocked(A11yRequest &request)
{
    if (!EnsureConnectedLocked())
        return TransportFailure(A11yError::kDisabled, "helper_unavailable");
    int fd = socketFd_.load();
    if (!WriteFrame(fd, EncodeRequest(request))) {
        DisconnectLocked();
        return TransportFailure(A11yError::kDisabled, "write_failed");
    }
    std::string frame;
    if (!ReadFrame(fd, frame, request.timeoutMs + kReadSlackMs)) {
        DisconnectLocked();
        return TransportFailure(A11yError::kTimeout, "read_timeout");
    }
    A11yResponse response;
    if (!DecodeResponse(frame, response)) {
        DisconnectLocked();
        return TransportFailure(A11yError::kDisabled, "decode_failed");
    }
    // Note: like the original, response.id is not compared with request.id.
    return response;
}

bool Client::EnsureReady()
{
    std::lock_guard<std::mutex> lock(g_clientMutex);
    return EnsureConnectedLocked();
}

void Client::Disconnect()
{
    std::lock_guard<std::mutex> lock(g_clientMutex);
    DisconnectLocked();
}

void Client::BeginShutdown()
{
    NativeChild::StopRespawn(NativeChild::Kind::kAtspiHelper);
    // Unblock a call that is currently waiting on the socket (without the
    // mutex), then drop the connection under the mutex.
    int fd = socketFd_.load();
    if (fd >= 0)
        ::shutdown(fd, SHUT_RDWR);
    std::lock_guard<std::mutex> lock(g_clientMutex);
    DisconnectLocked();
}

void Client::TerminateHelper()
{
    BeginShutdown();
    NativeChild::Terminate(NativeChild::Kind::kAtspiHelper);
}

void Client::SetRetryAfterReadTimeout(bool enabled)
{
    g_retryAfterReadTimeout.store(enabled);
}

A11yResponse Client::Call(A11yRequest request)
{
    std::lock_guard<std::mutex> lock(g_clientMutex);
    if (request.timeoutMs <= 0)
        request.timeoutMs = kDefaultTimeoutMs;
    request.id = nextId_++;
    A11yResponse response = CallOnceLocked(request);
    bool retry = (response.error == A11yError::kTimeout && g_retryAfterReadTimeout.load()) ||
                 response.failReason == "write_failed" ||
                 response.failReason == "helper_unavailable";
    if (!retry)
        return response;
    DisconnectLocked();
    if (NativeChild::RespawnStopped(NativeChild::Kind::kAtspiHelper))
        return response;
    request.id = nextId_++;
    return CallOnceLocked(request);
}

bool Client::EnableSessionA11yStatus(bool reassert)
{
    A11yRequest r = MakeRequest(kCommandEnableSessionA11yStatus);
    r.reassert = reassert;
    return Call(std::move(r)).ok;
}

void Client::NoteForegroundProcessId(int processId)
{
    A11yRequest r = MakeRequest(kCommandNoteForegroundProcessId);
    r.processId = processId;
    Call(std::move(r));
}

void Client::NoteOwnProcessForeground()
{
    Call(MakeRequest(kCommandNoteOwnProcessForeground));
}

void Client::ReleaseCompositorForegroundLock()
{
    Call(MakeRequest(kCommandReleaseCompositorForegroundLock));
}

A11yResponse Client::GetActiveWindowSnapshot(int timeoutMs, int processId, ForegroundNote note)
{
    A11yRequest r = MakeRequest(kCommandGetActiveWindowSnapshot);
    r.timeoutMs = timeoutMs;
    r.processId = processId;
    switch (note) {
    case ForegroundNote::kLock: r.foregroundNote = "lock"; break;
    case ForegroundNote::kOwn: r.foregroundNote = "own"; break;
    case ForegroundNote::kRelease: r.foregroundNote = "release"; break;
    default: r.foregroundNote.clear(); break;
    }
    return Call(std::move(r));
}

A11yResponse Client::GetActiveWindowSnapshot(int timeoutMs)
{
    return GetActiveWindowSnapshot(timeoutMs, 0, ForegroundNote::kNone);
}

A11yResponse Client::GetFocusSnapshot(int timeoutMs)
{
    A11yRequest r = MakeRequest(kCommandGetFocusSnapshot);
    r.timeoutMs = timeoutMs;
    return Call(std::move(r));
}

A11yResponse Client::GetSurroundingText(int beforeChars, int afterChars, int timeoutMs,
                                        const FocusKey &focusKey)
{
    A11yRequest r = MakeRequest(kCommandGetSurroundingText);
    r.timeoutMs = timeoutMs;
    r.beforeChars = beforeChars;
    r.afterChars = afterChars;
    r.busName = focusKey.busName;
    r.objectPath = focusKey.objectPath;
    r.generation = focusKey.generation;
    return Call(std::move(r));
}

A11yResponse Client::GetVisibleText(int maxLength, int timeoutMs)
{
    A11yRequest r = MakeRequest(kCommandGetVisibleText);
    r.timeoutMs = timeoutMs;
    r.maxLength = maxLength;
    return Call(std::move(r));
}

A11yResponse Client::FindDocumentUrlInActiveWindow(const std::string &windowTitle, int processId,
                                                   int timeoutMs)
{
    A11yRequest r = MakeRequest(kCommandFindDocumentUrlInActiveWindow);
    r.windowTitle = windowTitle;
    r.timeoutMs = timeoutMs;
    r.processId = processId;
    return Call(std::move(r));
}

A11yResponse Client::ReadFocusedInputState(int timeoutMs)
{
    A11yRequest r = MakeRequest(kCommandReadFocusedInputState);
    r.timeoutMs = timeoutMs;
    return Call(std::move(r));
}

Atspi::FocusedInputCapability Client::ProbeFocusedInputCapability()
{
    A11yResponse resp = Call(MakeRequest(kCommandProbeFocusedInputCapability));
    if (!resp.ok)
        return Atspi::FocusedInputCapability::kUnknown;
    std::string capability;
    JsonReadString(resp.payloadJson, "capability", capability);
    if (capability == "editable")
        return Atspi::FocusedInputCapability::kEditable;
    if (capability == "not-editable")
        return Atspi::FocusedInputCapability::kNotEditable;
    return Atspi::FocusedInputCapability::kUnknown;
}

bool Client::IsDesktopReachable()
{
    A11yResponse resp = Call(MakeRequest(kCommandIsDesktopReachable));
    if (!resp.ok)
        return false;
    bool reachable = false;
    if (!JsonReadBool(resp.payloadJson, "reachable", reachable))
        return false;
    return reachable;
}

bool Client::RecoverAfterResume()
{
    return Call(MakeRequest(kCommandRecoverAfterResume)).ok;
}

}  // namespace A11y
}  // namespace LinuxShared
