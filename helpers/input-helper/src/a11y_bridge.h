// Thin adapter between libInputHelper and the shared typeless-atspi-helper
// client (helpers/linux-shared). Keeps the reader independent of the client's
// exact C++ API.
#pragma once

#include <string>

namespace InputHelper {
namespace A11yBridge {

struct Result {
    bool ok = false;
    std::string error;       // helper-side error code ("disabled", "defunct", ...)
    std::string failReason;  // transport failure ("helper_unavailable", "read_timeout", ...)
    std::string payload;     // JSON object text
};

void EnsureReady();
Result ReadFocusedInputState(int timeoutMs);
void RecoverAfterResume();
void Shutdown();  // stop respawning, disconnect

}  // namespace A11yBridge
}  // namespace InputHelper
