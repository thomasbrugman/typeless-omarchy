// Fallback bridge used when the shared AT-SPI client is not available at build
// time: every query fails with "helper_unavailable".
#include "a11y_bridge.h"

namespace InputHelper {
namespace A11yBridge {

void EnsureReady() {}

Result ReadFocusedInputState(int)
{
    Result r;
    r.ok = false;
    r.failReason = "helper_unavailable";
    return r;
}

void RecoverAfterResume() {}
void Shutdown() {}

}  // namespace A11yBridge
}  // namespace InputHelper
