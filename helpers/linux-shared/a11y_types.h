// a11y_types.h - request/response types shared by the typeless-atspi-helper
// server and the in-process client (interoperable reimplementation of Typeless'
// linux-shared/a11y_types.h + atspi_walk_limits.h; field layout matches the
// x64 DWARF).
#pragma once

#include <cstdint>
#include <string>

namespace LinuxShared {
namespace Atspi {

// All limits default to 0 = "use the handler's built-in default".
struct TreeWalkLimits {
    int maxDepth = 0;
    int maxBreadth = 0;
    int maxNodes = 0;
};

struct ParentWalkLimits {
    int maxLevels = 0;
};

struct EditableSearchLimits {
    int maxAncestorLevels = 0;
    int maxDescendantNodes = 0;
    int maxBreadth = 0;
    int maxInputTextLength = 0;
};

struct TextReadLimits {
    int maxCharsPerCall = 0;
};

enum class FocusedInputCapability : int {
    kUnknown = 0,
    kEditable = 1,
    kNotEditable = 2,
};

}  // namespace Atspi

namespace A11y {

enum class A11yError : int {
    kOk = 0,
    kUnavailable = 1,
    kTimeout = 2,
    kDefunct = 3,
    kAborted = 4,
    kDisabled = 5,
    kInvalid = 6,
};

enum class ForegroundNote : int {
    kNone = 0,
    kLock = 1,
    kOwn = 2,
    kRelease = 3,
};

struct FocusKey {
    std::string busName;
    std::string objectPath;
    uint64_t generation = 0;
};

// Wire command names ("cmd").
inline constexpr const char kCommandEnableSessionA11yStatus[] = "EnableSessionA11yStatus";
inline constexpr const char kCommandNoteForegroundProcessId[] = "NoteForegroundProcessId";
inline constexpr const char kCommandNoteOwnProcessForeground[] = "NoteOwnProcessForeground";
inline constexpr const char kCommandReleaseCompositorForegroundLock[] = "ReleaseCompositorForegroundLock";
inline constexpr const char kCommandGetActiveWindowSnapshot[] = "GetActiveWindowSnapshot";
inline constexpr const char kCommandGetFocusSnapshot[] = "GetFocusSnapshot";
inline constexpr const char kCommandGetSurroundingText[] = "GetSurroundingText";
inline constexpr const char kCommandGetVisibleText[] = "GetVisibleText";
inline constexpr const char kCommandFindDocumentUrlInActiveWindow[] = "FindDocumentUrlInActiveWindow";
inline constexpr const char kCommandReadFocusedInputState[] = "ReadFocusedInputState";
inline constexpr const char kCommandProbeFocusedInputCapability[] = "ProbeFocusedInputCapability";
inline constexpr const char kCommandIsDesktopReachable[] = "IsDesktopReachable";
inline constexpr const char kCommandRecoverAfterResume[] = "RecoverAfterResume";
inline constexpr const char kCommandShutdown[] = "Shutdown";

inline constexpr const char kHelperBinaryName[] = "typeless-atspi-helper";

struct A11yRequest {
    uint64_t id = 0;
    std::string command;
    int timeoutMs = 300;
    int processId = 0;
    int beforeChars = 0;
    int afterChars = 0;
    int maxLength = 0;
    bool reassert = false;
    std::string windowTitle;
    std::string busName;
    std::string objectPath;
    uint64_t generation = 0;
    std::string foregroundNote;  // "", "lock", "own", "release"
    Atspi::TreeWalkLimits walkLimits;
    Atspi::ParentWalkLimits parentWalkLimits;
    Atspi::EditableSearchLimits editableSearchLimits;
    Atspi::TextReadLimits textReadLimits;
    int focusScanTimeoutMs = 0;
};

struct A11yResponse {
    uint64_t id = 0;
    bool ok = false;
    A11yError error = A11yError::kInvalid;
    std::string failReason;
    uint64_t generation = 0;
    std::string payloadJson;  // always a JSON object text ("{}" when empty)
};

}  // namespace A11y
}  // namespace LinuxShared
