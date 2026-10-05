// Shared helpers for the arm64 reimplementation of libInputHelper.so.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace InputHelper {

// ---- logging (stderr, like the original DebugLog; never logs text content) ----
void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- lifecycle ----
bool IsShuttingDown();
void SetShuttingDown(bool value);

// ---- time ----
int64_t NowMs();
void SleepMs(int ms);  // EINTR-safe

// ---- string utils (mirror string_utils.cpp) ----
std::string CleanZeroWidthChars(const std::string &text);                       // drops U+200B
size_t Utf8CodePointCount(const std::string &text);
std::string LimitTextLength(const std::string &text, size_t maxCodePoints, bool keepTail);
std::string EscapeHtml(const std::string &text);
std::string HtmlToPlainText(const std::string &html);
std::string JsonEscape(const std::string &text);

// ---- tiny JSON reader (enough for helper payloads / hyprctl output) ----
struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JsonValue> a;
    std::map<std::string, JsonValue> o;

    const JsonValue *Get(const std::string &key) const;
    std::string GetString(const std::string &key, const std::string &def = "") const;
    bool GetBool(const std::string &key, bool def = false) const;
    double GetNumber(const std::string &key, double def = 0) const;
};
bool JsonParse(const std::string &text, JsonValue &out);

// ---- process helpers ----
// Directory containing libInputHelper.so (resolved with dladdr).
std::string LibraryDirectory();
// Environment for spawned helpers with launcher/AppImage leftovers removed.
std::vector<std::string> SanitizedChildEnvironment();

struct SpawnResult {
    bool started = false;
    bool exited = false;      // exited before the deadline
    int exitCode = -1;        // valid if exited && !signaled
    bool signaled = false;
    std::string out;          // captured stdout (bounded)
};
// Runs argv[0] (PATH lookup when it has no '/'), feeds `input` on stdin, captures
// stdout, and waits up to timeoutMs for exit. On timeout the child is SIGKILLed.
SpawnResult RunProcess(const std::vector<std::string> &argv, const std::string &input, int timeoutMs,
                       size_t maxOutput = 128u * 1024u * 1024u);
bool CommandExists(const char *name);  // cached PATH lookup

}  // namespace InputHelper
