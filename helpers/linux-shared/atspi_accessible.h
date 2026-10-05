// atspi_accessible.h - AT-SPI access layer of typeless-atspi-helper
// (interoperable reimplementation of Typeless' linux-shared/atspi_accessible.cpp,
// atspi_api.h, atspi_session.h and a11y_apartment.h).
//
// Everything here runs on the helper's single thread. All libatspi traffic
// goes through a private GMainContext (the "apartment") that is pushed as the
// thread default; it is pumped between requests (PumpEvents) and after
// requests (DrainPendingCacheUpdates) so focus/window events keep the caches
// fresh. Every D-Bus method call is bounded by atspi_set_timeout(300 ms).
#pragma once

#include <atspi/atspi.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "a11y_types.h"

namespace LinuxShared {

namespace AtspiApi {
// Loads libatspi.so.0 (dlopen) and the optional symbols. false = unusable.
bool Load();
bool IsLoaded();
}  // namespace AtspiApi

namespace Atspi {

inline constexpr int kMethodTimeoutMs = 300;
inline constexpr int kStartupTimeoutMs = 2000;
inline constexpr int kPumpEventsMaxIterations = 384;
inline constexpr int kPumpEventsMaxMs = 120;

// Deadline helper (CreateTimeoutChecker in the original).
class Deadline {
public:
    explicit Deadline(int timeoutMs);
    bool Expired() const;
    int RemainingMs() const;

private:
    std::chrono::steady_clock::time_point end_;
    bool infinite_;
};

enum class CollectionWalkStatus { kContinue = 0, kSkipChildren = 1, kStop = 2 };

bool EnsureInitialized();
bool IsReady();
// Advertises session accessibility (org.a11y.Status IsEnabled=true and, when
// the GNOME schema exists and it is off, toolkit-accessibility=true).
// Returns whether accessibility is advertised as enabled.
bool EnableSessionAccessibility(bool reassert);
bool IsDesktopReachable();
bool EnsureAccessibleReady();
void PumpEvents();
void DrainPendingCacheUpdates();
uint64_t CollectionCacheEpoch();
void ReleaseAccessibleCaches();
void RecoverAfterResume();

// Foreground hints from the host (compositor pid lock etc.).
void NoteForegroundProcessId(int pid);
void NoteOwnProcessForeground();
void ReleaseCompositorForegroundLock();
int ForegroundProcessId();  // 0 = unknown

// RAII "session": true when the AT-SPI runtime is usable.
class Session {
public:
    Session();
    ~Session();
    explicit operator bool() const { return ok_; }

private:
    bool ok_;
};

// ---- accessor helpers (all tolerate nullptr; strings are UTF-8) ----
bool IsLiveAtspiAccessible(AtspiAccessible *obj);
bool IsDefunctAccessible(AtspiAccessible *obj);
AtspiRole GetRole(AtspiAccessible *obj);
std::string GetName(AtspiAccessible *obj);
std::string GetDescription(AtspiAccessible *obj);
AtspiStateSet *GetStateSet(AtspiAccessible *obj);  // caller unrefs
bool HasState(AtspiAccessible *obj, AtspiStateType state);
bool StateSetContains(AtspiStateSet *set, AtspiStateType state);
int CachedProcessId(AtspiAccessible *obj);
AtspiAccessible *GetParent(AtspiAccessible *obj);  // new ref or nullptr
int GetChildCount(AtspiAccessible *obj);
AtspiAccessible *GetChildAtIndex(AtspiAccessible *obj, int index);  // new ref
AtspiText *GetTextInterface(AtspiAccessible *obj);                   // new ref
bool HasEditableTextInterface(AtspiAccessible *obj);
int GetTextCharacterCount(AtspiText *text);
int GetTextCaretOffset(AtspiText *text);
// Reads [start,end) in chunks of <= maxCharsPerCall characters.
bool ReadAccessibleText(AtspiText *text, int start, int end, int maxCharsPerCall,
                        std::string &out);
void ReadAccessibleFocusKey(AtspiAccessible *obj, std::string &busName,
                            std::string &objectPath);
bool IsEditableFromSnapshot(AtspiStateSet *set, AtspiRole role);
bool IsExplicitlyNonTextControl(AtspiAccessible *obj);
bool IsOwnProcessId(int pid);

// Breadth-first walk of obj's children (not obj itself). The visitor gets a
// borrowed pointer. Returns false when aborted by deadline/limits.
bool VisitChildren(AtspiAccessible *root, const TreeWalkLimits &limits,
                   const std::function<bool()> &shouldAbort,
                   const std::function<CollectionWalkStatus(AtspiAccessible *)> &visit);

// Active top-level window (new ref) and the focused accessible (new ref).
AtspiAccessible *FindActiveWindow();
AtspiAccessible *FindTopLevelWindowOf(AtspiAccessible *obj, int maxLevels);  // new ref
AtspiAccessible *GetFocusedAccessible(int timeoutMs, const TreeWalkLimits &limits);

struct FocusedInputState {
    bool valid = false;
    std::string error;
    std::string beforeText;
    std::string selectedText;
    std::string afterText;
};

FocusedInputState ReadFocusedInputState(const EditableSearchLimits &limits);
FocusedInputCapability ProbeFocusedInputCapability(const EditableSearchLimits &limits);

// Finds the editable text field for the current focus (new ref) or nullptr
// with *error set ("no_focused_element", "not_editable", "timeout", ...).
AtspiAccessible *LookupFocusedEditable(const char *tag, std::string *error,
                                       const EditableSearchLimits &limits, int timeoutMs);

}  // namespace Atspi
}  // namespace LinuxShared
