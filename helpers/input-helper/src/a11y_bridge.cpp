// A11yBridge on top of the shared typeless-atspi-helper client
// (helpers/linux-shared/a11y_client.*, wire compatible with the original).
#include "a11y_bridge.h"

#include <atomic>
#include <thread>

#include "a11y_client.h"
#include "a11y_ipc.h"

namespace InputHelper {
namespace A11yBridge {
namespace {

using LinuxShared::A11y::A11yResponse;
using LinuxShared::A11y::Client;

std::atomic<bool> g_warming{false};
std::atomic<bool> g_configured{false};

void ConfigureOnce()
{
    if (!g_configured.exchange(true)) {
        // The JS side gives up after 500 ms; a second read_timeout round trip
        // could never be used, so keep every call to one round trip.
        Client::SetRetryAfterReadTimeout(false);
    }
}

}  // namespace

void EnsureReady()
{
    // Original calls Client::EnsureReady() on the calling thread, which is the
    // app's JS thread (koffi ignores the app's {async:true}); spawning the helper
    // can take a while, so warm it up in the background instead.
    ConfigureOnce();
    if (g_warming.exchange(true)) return;
    try {
        std::thread([] {
            Client::Instance().EnsureReady();
            g_warming.store(false);
        }).detach();
    } catch (...) {
        g_warming.store(false);
    }
}

Result ReadFocusedInputState(int timeoutMs)
{
    ConfigureOnce();
    A11yResponse response = Client::Instance().ReadFocusedInputState(timeoutMs);
    Result r;
    r.ok = response.ok;
    if (!response.ok) r.error = LinuxShared::A11y::A11yErrorName(response.error);
    r.failReason = response.failReason;
    r.payload = response.payloadJson.empty() ? "{}" : response.payloadJson;
    return r;
}

void RecoverAfterResume()
{
    Client::Instance().RecoverAfterResume();
}

void Shutdown()
{
    Client::Instance().BeginShutdown();
}

}  // namespace A11yBridge
}  // namespace InputHelper
