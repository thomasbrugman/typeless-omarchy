// a11y_handlers.h - request dispatch for typeless-atspi-helper (clean-room
// reimplementation of Typeless' linux-shared/a11y_handlers.cpp).
#pragma once

#include "a11y_types.h"

namespace LinuxShared {
namespace A11y {

// Runs one request (inside an Atspi::Session). Never throws.
A11yResponse HandleRequest(const A11yRequest &request);
A11yResponse DispatchA11yCommand(const A11yRequest &request);

// Called by the main loop between requests.
void PumpApartmentEvents();
void DrainCacheUpdates();

}  // namespace A11y
}  // namespace LinuxShared
