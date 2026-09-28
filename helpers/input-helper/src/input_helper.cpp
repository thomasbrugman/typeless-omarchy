// Exported C ABI of libInputHelper.so (clean-room arm64 port of Typeless 2.2.0).
//
// JS binding (koffi 2.14.1, bootstrapPrimaryInstance ~7700-7960). The app passes
// `{async: true}` as a 4th lib.func() argument, which koffi ignores: every call
// below runs synchronously on Electron's main thread, so nothing here may block
// for long or wait on a thread that could be relaying a callback to JS.
//   int32 insertText(str)                       1 = pasted, 0 = not (empty/null/failure)
//   int32 insertRichText(str html, str text)    1 = pasted, 0 = not
//   int32 deleteBackward(int32 count)           0 ok, -1 bad count, -2 failed, -3 exception
//   str   getSelectedText()                     (JS doesn't call it)
//   str   getCurrentInputState()                (JS doesn't call it)
//   void  getSelectedTextAsync(cb)              cb(str) once: selected text or ""
//   void  getCurrentInputStateAsync(cb)         cb(str) once: JSON (see ToJson)
//   void  getSelectedTextBySimulateCopyAsync(cb) cb(str) once: copied text or ""
//   void  cleanup_input_helper()
//   void  recover_input_helper_after_resume()
//   int32 warmup_voice_ime() / restore_voice_ime()  1 = IME active/restored
//   str   testTSFFunction()                     diagnostic JSON
//
// Returned `str` pointers stay owned by the library (thread-local buffers valid
// until the next call on the same thread); koffi copies them immediately and
// never frees plain `str` returns. Callback strings are only valid during the
// call; koffi relays calls from our threads to the JS thread and blocks until
// the JS function returns, so each callback is invoked exactly once and never
// after cleanup started.
#include <stdint.h>
#include <string.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#include "accessibility_reader.h"
#include "async_executor.h"
#include "clipboard.h"
#include "common.h"
#include "focused_window.h"
#include "keyboard_inject.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

typedef void (*InputHelperCallback)(const char *);

using namespace InputHelper;

namespace {

// Serializes every clipboard save/set/paste/restore sequence. Leaked on purpose
// (detached restore/copy threads may outlive static destruction at exit).
std::mutex &g_clipboardOperationMutex = *new std::mutex();

// A restore that has been scheduled but not run yet. A new paste inside the
// restore window reuses this snapshot instead of saving our own transcript.
struct PendingRestore {
    bool active = false;
    uint64_t generation = 0;
    Clipboard::Snapshot snapshot;
    std::string ourText;  // plain text we put on the clipboard
};
PendingRestore &g_pending = *new PendingRestore();  // guarded by g_clipboardOperationMutex
uint64_t g_generation = 0;

const char *ReturnString(const std::string &value)
{
    static thread_local std::string buffer;
    buffer = value;
    return buffer.c_str();
}

void InvokeCallback(InputHelperCallback cb, const std::string &value)
{
    if (!cb) return;
    std::string copy = value;  // keep alive for the duration of the call
    cb(copy.c_str());
}

// Caller holds g_clipboardOperationMutex.
Clipboard::Snapshot TakeSnapshotLocked()
{
    if (g_pending.active) {
        g_pending.active = false;  // the pending thread sees the generation change
        g_generation++;
        return g_pending.snapshot;
    }
    return Clipboard::Save();
}

// Caller holds g_clipboardOperationMutex.
void RestoreClipboardLaterLocked(const Clipboard::Snapshot &snapshot, const std::string &ourText, int delayMs)
{
    uint64_t gen = ++g_generation;
    g_pending.active = true;
    g_pending.generation = gen;
    g_pending.snapshot = snapshot;
    g_pending.ourText = ourText;
    std::thread([gen, delayMs] {
        SleepMs(delayMs);
        if (IsShuttingDown()) return;
        std::lock_guard<std::mutex> lock(g_clipboardOperationMutex);
        if (IsShuttingDown() || !g_pending.active || g_pending.generation != gen) return;
        g_pending.active = false;
        // Don't clobber something the user copied in the meantime.
        if (Clipboard::GetText() != g_pending.ourText) {
            Log("[InputHelper] Clipboard changed since paste, restore skipped");
            return;
        }
        Clipboard::Restore(g_pending.snapshot);
        Log("[InputHelper] Clipboard restored");
    }).detach();
}

KeyboardInject::Chord PasteChordForFocusedWindow()
{
    FocusedWindow::Info info = FocusedWindow::Query(150);
    if (FocusedWindow::IsTerminal(info)) {
        Log("[insertText] terminal target class=%s, using Ctrl+Shift+V", info.windowClass.c_str());
        return KeyboardInject::Chord::CtrlShiftV;
    }
    return KeyboardInject::Chord::CtrlV;
}

KeyboardInject::Chord CopyChordForFocusedWindow()
{
    FocusedWindow::Info info = FocusedWindow::Query(150);
    if (FocusedWindow::IsTerminal(info)) {
        Log("[InputHelper] terminal target class=%s, using Ctrl+Shift+C", info.windowClass.c_str());
        return KeyboardInject::Chord::CtrlShiftC;
    }
    return KeyboardInject::Chord::CtrlC;
}

std::string TestTsfJson()
{
    long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
    return "{\"timestamp\":\"" + std::to_string(ms) +
           "\",\"tsf_initialized\":false,\"is_in_editable_tsf\":false,\"platform\":\"linux\"}";
}

}  // namespace

EXPORT int32_t insertText(const char *text)
{
    if (!text) {
        Log("[insertText] ERROR: text is nullptr");
        return 0;
    }
    try {
        std::string value(text);
        if (value.empty()) {
            Log("[insertText] ERROR: text is empty");
            return 0;
        }
        Log("[insertText] Starting insertion, length=%zu", value.size());
        // The original first tries an IBus / Fcitx VoiceIME commit. Neither engine
        // is available on this port, which is also what the original concludes on
        // a Hyprland + fcitx5 session, so go straight to clipboard + paste.
        Log("[insertText] backend=clipboard-paste status=starting bytes=%zu", value.size());
        KeyboardInject::Chord chord = PasteChordForFocusedWindow();

        std::lock_guard<std::mutex> lock(g_clipboardOperationMutex);
        bool resumedPending = g_pending.active;
        Clipboard::Snapshot saved = TakeSnapshotLocked();
        if (!Clipboard::SetText(value)) {
            Log("[insertText] backend=clipboard-paste status=failed stage=set-clipboard");
            if (resumedPending) RestoreClipboardLaterLocked(saved, g_pending.ourText, 0);
            return 0;
        }
        SleepMs(30);
        bool pasted = KeyboardInject::SimulatePaste(chord);
        Log("[insertText] backend=clipboard-paste status=%s bytes=%zu clipboardRestoreDelayMs=500",
            pasted ? "succeeded" : "failed", value.size());
        RestoreClipboardLaterLocked(saved, value, 500);
        return pasted ? 1 : 0;
    } catch (...) {
        Log("[insertText] EXCEPTION");
        return 0;
    }
}

EXPORT int32_t insertRichText(const char *html, const char *text)
{
    try {
        std::string htmlValue = html ? html : "";
        std::string textValue = text ? text : "";
        if (htmlValue.empty() && textValue.empty()) {
            Log("[insertRichText] WARNING: No content provided");
            return 0;
        }
        if (htmlValue.empty()) htmlValue = "<p>" + EscapeHtml(textValue) + "</p>";
        if (textValue.empty()) textValue = HtmlToPlainText(htmlValue);
        Log("[insertRichText] backend=rich-clipboard-paste status=starting htmlBytes=%zu textBytes=%zu",
            htmlValue.size(), textValue.size());
        KeyboardInject::Chord chord = PasteChordForFocusedWindow();

        std::lock_guard<std::mutex> lock(g_clipboardOperationMutex);
        bool resumedPending = g_pending.active;
        Clipboard::Snapshot saved = TakeSnapshotLocked();
        if (!Clipboard::SetRichText(htmlValue, textValue)) {
            Log("[insertRichText] backend=rich-clipboard-paste status=failed stage=set-clipboard");
            if (resumedPending) RestoreClipboardLaterLocked(saved, g_pending.ourText, 0);
            return 0;
        }
        SleepMs(30);
        bool pasted = KeyboardInject::SimulatePaste(chord);
        Log("[insertRichText] backend=rich-clipboard-paste status=%s htmlBytes=%zu textBytes=%zu "
            "clipboardRestoreDelayMs=1000",
            pasted ? "succeeded" : "failed", htmlValue.size(), textValue.size());
        RestoreClipboardLaterLocked(saved, textValue, 1000);
        return pasted ? 1 : 0;
    } catch (...) {
        Log("[insertRichText] EXCEPTION");
        return 0;
    }
}

EXPORT int32_t deleteBackward(int32_t count)
{
    if (count <= 0) return -1;
    try {
        return KeyboardInject::SimulateBackspace(count) ? 0 : -2;
    } catch (...) {
        return -3;
    }
}

EXPORT const char *getSelectedText()
{
    try {
        return ReturnString(AccessibilityReader::GetSelectedText());
    } catch (...) {
        return ReturnString("");
    }
}

EXPORT const char *getCurrentInputState()
{
    Log("[InputHelper] getCurrentInputState START");
    try {
        std::string json = AccessibilityReader::ToJson(AccessibilityReader::GetCurrentInputState());
        Log("[InputHelper] getCurrentInputState done, length=%zu", json.size());
        return ReturnString(json);
    } catch (...) {
        return ReturnString("{\"error\":\"exception_occurred\"}");
    }
}

EXPORT const char *testTSFFunction()
{
    return ReturnString(TestTsfJson());
}

EXPORT void getSelectedTextAsync(InputHelperCallback callback)
{
    if (!callback) {
        Log("[InputHelper] ERROR: callback is nullptr");
        return;
    }
    try {
        AccessibilityReader::Initialize();
        AsyncExecutor::PostWithEvict(
            AsyncExecutor::TaskKind::SelectedText,
            [callback] {
                std::string text;
                try {
                    text = AccessibilityReader::GetSelectedText();
                } catch (...) {
                    text.clear();
                }
                if (IsShuttingDown()) return;
                InvokeCallback(callback, text);
            },
            [callback] { InvokeCallback(callback, ""); });
    } catch (...) {
        InvokeCallback(callback, "");
    }
}

EXPORT void getCurrentInputStateAsync(InputHelperCallback callback)
{
    if (!callback) {
        Log("[InputHelper] ERROR: callback is nullptr");
        return;
    }
    try {
        AccessibilityReader::Initialize();
        AsyncExecutor::PostWithEvict(
            AsyncExecutor::TaskKind::InputState,
            [callback] {
                std::string json;
                try {
                    AccessibilityReader::InputState state = AccessibilityReader::GetCurrentInputState();
                    json = AccessibilityReader::ToJson(state);
                    Log("[InputHelper] getCurrentInputStateAsync valid=%s error=%s jsonBytes=%zu",
                        state.valid ? "true" : "false", state.error.c_str(), json.size());
                } catch (...) {
                    json = "{\"error\":\"exception_occurred\"}";
                }
                if (IsShuttingDown()) return;
                InvokeCallback(callback, json);
            },
            [callback] { InvokeCallback(callback, "{\"error\":\"evicted\"}"); });
    } catch (...) {
        InvokeCallback(callback, "{\"error\":\"exception_occurred\"}");
    }
}

EXPORT void getSelectedTextBySimulateCopyAsync(InputHelperCallback callback)
{
    if (!callback) {
        Log("[InputHelper] ERROR: callback is nullptr");
        return;
    }
    if (IsShuttingDown()) {
        InvokeCallback(callback, "");
        return;
    }
    Log("[InputHelper] getSelectedTextBySimulateCopyAsync START");
    try {
        std::thread([callback] {
            if (IsShuttingDown()) return;
            std::string result;
            try {
                KeyboardInject::Chord chord = CopyChordForFocusedWindow();
                std::string copied;
                {
                    std::lock_guard<std::mutex> lock(g_clipboardOperationMutex);
                    Clipboard::Snapshot saved = TakeSnapshotLocked();
                    Clipboard::Clear();
                    SleepMs(50);
                    if (KeyboardInject::SimulateCopy(chord)) {
                        // Original waits a flat 200 ms; poll a little longer for slow apps.
                        int64_t deadline = NowMs() + 450;
                        SleepMs(80);
                        for (;;) {
                            copied = Clipboard::GetText();
                            if (!copied.empty() || NowMs() >= deadline) break;
                            SleepMs(60);
                        }
                    } else {
                        Log("[InputHelper] SimulateCopy failed");
                    }
                    Clipboard::Restore(saved);
                }
                result = LimitTextLength(CleanZeroWidthChars(copied), 5000, false);
                Log("[InputHelper] getSelectedTextBySimulateCopyAsync done, length=%zu", result.size());
            } catch (...) {
                Log("[InputHelper] getSelectedTextBySimulateCopyAsync exception");
                result.clear();
            }
            if (IsShuttingDown()) return;
            InvokeCallback(callback, result);
        }).detach();
    } catch (...) {
        InvokeCallback(callback, "");
    }
}

EXPORT void cleanup_input_helper()
{
    SetShuttingDown(true);
    // Put the user's clipboard back now rather than leaving our transcript on it.
    {
        std::lock_guard<std::mutex> lock(g_clipboardOperationMutex);
        if (g_pending.active) {
            g_pending.active = false;
            g_generation++;
            if (Clipboard::GetText() == g_pending.ourText) Clipboard::Restore(g_pending.snapshot);
        }
    }
    KeyboardInject::Shutdown();
    AsyncExecutor::Shutdown();
    AccessibilityReader::Cleanup();
}

EXPORT void recover_input_helper_after_resume()
{
    // Called synchronously on the JS thread: never block it.
    if (IsShuttingDown()) return;
    try {
        std::thread([] {
            try {
                AccessibilityReader::RecoverAfterResume();
            } catch (...) {
            }
        }).detach();
    } catch (...) {
    }
}

EXPORT int32_t warmup_voice_ime()
{
    // No IBus/Fcitx VoiceIME engine on this port: never switch the user's IME.
    // The app calls this once at startup (synchronously on the JS thread), so use
    // it to create the virtual keyboard in the background: the compositor needs
    // ~200 ms to enumerate a new device, which would otherwise delay the first
    // paste on the JS thread.
    static std::once_flag once;
    std::call_once(once, [] {
        try {
            std::thread([] {
                if (!IsShuttingDown()) KeyboardInject::EnsureDevice();
            }).detach();
        } catch (...) {
        }
    });
    return 0;
}

EXPORT int32_t restore_voice_ime()
{
    return 0;
}
