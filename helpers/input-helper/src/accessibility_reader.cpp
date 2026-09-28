// Focused-field state via the out-of-process AT-SPI helper (original:
// accessibility_reader.cpp). The original falls back to the IBus VoiceIME
// "surrounding" query when AT-SPI fails; that engine is not used on this port.
#include "accessibility_reader.h"

#include "a11y_bridge.h"
#include "common.h"

namespace InputHelper {
namespace AccessibilityReader {
namespace {

const int kReadTimeoutMs = 300;
const size_t kMaxChars = 5000;

InputState MapFocusedInputState(bool valid, const std::string &error, const std::string &before,
                                const std::string &selected, const std::string &after)
{
    InputState state;
    state.valid = valid;
    state.error = error;
    if (!valid) return state;
    state.beforeText = LimitTextLength(CleanZeroWidthChars(before), kMaxChars, true);
    state.selectedText = LimitTextLength(CleanZeroWidthChars(selected), kMaxChars, false);
    state.afterText = LimitTextLength(CleanZeroWidthChars(after), kMaxChars, false);
    state.startIndex = (int)Utf8CodePointCount(state.beforeText);
    state.endIndex = state.startIndex + (int)Utf8CodePointCount(state.selectedText);
    return state;
}

}  // namespace

void Initialize()
{
    A11yBridge::EnsureReady();
}

InputState GetCurrentInputState()
{
    A11yBridge::Result r = A11yBridge::ReadFocusedInputState(kReadTimeoutMs);
    if (!r.ok) {
        std::string reason = !r.failReason.empty() ? r.failReason
                             : !r.error.empty()    ? r.error
                                                   : "atspi_initialization_failed";
        Log("[InputHelper] input state ipc failed reason=%s", reason.c_str());
        InputState state;
        state.valid = false;
        state.error = reason;
        return state;
    }
    JsonValue payload;
    JsonParse(r.payload, payload);
    InputState state = MapFocusedInputState(payload.GetBool("valid"), payload.GetString("error"),
                                            payload.GetString("beforeText"), payload.GetString("selectedText"),
                                            payload.GetString("afterText"));
    if (!state.valid) {
        if (state.error.empty()) state.error = "unknown";
        Log("[InputHelper] input state error=%s kept", state.error.c_str());
    }
    return state;
}

std::string GetSelectedText()
{
    A11yBridge::Result r = A11yBridge::ReadFocusedInputState(kReadTimeoutMs);
    if (!r.ok) return std::string();
    JsonValue payload;
    if (!JsonParse(r.payload, payload)) return std::string();
    if (!payload.GetBool("valid")) return std::string();
    std::string selected = payload.GetString("selectedText");
    if (selected.empty()) return std::string();
    return LimitTextLength(CleanZeroWidthChars(selected), kMaxChars, false);
}

std::string ToJson(const InputState &state)
{
    if (!state.valid) {
        return "{\"error\":\"" + JsonEscape(state.error.empty() ? "unknown_error" : state.error) + "\"}";
    }
    std::string out = "{\"startIndex\":" + std::to_string(state.startIndex) +
                      ",\"endIndex\":" + std::to_string(state.endIndex) + ",\"beforeText\":\"" +
                      JsonEscape(state.beforeText) + "\",\"selectedText\":\"" + JsonEscape(state.selectedText) +
                      "\",\"afterText\":\"" + JsonEscape(state.afterText) + "\"}";
    return out;
}

void RecoverAfterResume()
{
    A11yBridge::RecoverAfterResume();
}

void Cleanup()
{
    A11yBridge::Shutdown();
}

}  // namespace AccessibilityReader
}  // namespace InputHelper
