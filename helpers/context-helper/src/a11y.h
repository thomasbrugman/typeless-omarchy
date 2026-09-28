// In-process AT-SPI client speaking raw D-Bus (libdbus, private connections).
#pragma once

#include <stdbool.h>
#include "window.h"

// Idempotent. Starts the event-listener thread lazily on first connection.
void a11y_shutdown(void);                    // close connections, stop event thread
void a11y_recover_after_resume(void);        // drop connection + caches (non-blocking)

// org.a11y.Status IsEnabled/ScreenReaderEnabled (+ gsettings toolkit-accessibility).
// reassert=false only does it once per connection lifetime.
bool a11y_enable_session(bool reassert);
bool a11y_desktop_reachable(void);    // connects if needed
bool a11y_connected(void);            // non-blocking: connection currently up?
// setFocusedWindowEnhancedUserInterface() result from cached state:
// 1 ok/unknown, -1 accessibility bus unreachable, -2 enabling failed.
int a11y_enable_status_code(void);

// Fallback when Hyprland IPC is not available: the AT-SPI window with STATE_ACTIVE.
bool a11y_active_window(fg_window_t *out, int timeout_ms);

// Returns malloc()ed payload JSON of the focused element snapshot, or NULL if
// the accessibility bus is unavailable. `fg` may be NULL (queried internally).
char *a11y_focus_snapshot_json(int budget_ms);

// getFocusedElementRelatedContent payload (always non-NULL JSON).
char *a11y_related_content_json(int before, int after, int timeout_ms);

// Plain visible text of the foreground window ("" when unavailable).
char *a11y_visible_text(int max_len, int timeout_ms);

// URL of the page shown in the foreground browser window, "" if unknown.
char *a11y_browser_url(int pid, const char *window_title, int budget_ms);
