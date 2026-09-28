// Clipboard access for the paste path.
//
// Primary backend: the bundled `typeless-clipboard` tool (ext-data-control-v1,
// focus-free, several MIME types per source, forks to serve like wl-copy).
// Fallback: wl-copy / wl-paste (what the original shells out to), which only
// handle one MIME type at a time. X11-only sessions are not supported here.
#include "clipboard.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>

#include "common.h"

namespace InputHelper {
namespace Clipboard {
namespace {

const char kPlainUtf8[] = "text/plain;charset=utf-8";
const int kToolExitEmpty = 1;
const int kToolExitNoProtocol = 3;

// Sticky once the compositor proves it lacks ext-data-control.
std::atomic<bool> g_toolUnusable{false};

std::string ToolPath()
{
    const char *env = getenv("TYPELESS_CLIPBOARD_TOOL");
    if (env && *env) return env;
    return LibraryDirectory() + "/typeless-clipboard";
}

bool WaylandSession()
{
    const char *wd = getenv("WAYLAND_DISPLAY");
    return wd && *wd;
}

bool ToolAvailable()
{
    if (g_toolUnusable.load() || !WaylandSession()) return false;
    return access(ToolPath().c_str(), X_OK) == 0;
}

void AppendFrame(std::string &out, const std::string &mime, const std::string &data)
{
    uint32_t ml = (uint32_t)mime.size();
    uint64_t dl = data.size();
    out.append(reinterpret_cast<const char *>(&ml), sizeof ml);
    out += mime;
    out.append(reinterpret_cast<const char *>(&dl), sizeof dl);
    out += data;
}

bool ParseFrames(const std::string &in, std::vector<std::pair<std::string, std::string>> &entries)
{
    size_t i = 0;
    while (i < in.size()) {
        uint32_t ml;
        uint64_t dl;
        if (i + sizeof ml > in.size()) return false;
        memcpy(&ml, in.data() + i, sizeof ml);
        i += sizeof ml;
        if (i + ml + sizeof dl > in.size()) return false;
        std::string mime = in.substr(i, ml);
        i += ml;
        memcpy(&dl, in.data() + i, sizeof dl);
        i += sizeof dl;
        if (dl > in.size() - i) return false;
        entries.emplace_back(std::move(mime), in.substr(i, (size_t)dl));
        i += (size_t)dl;
    }
    return true;
}

void NoteToolResult(const SpawnResult &r)
{
    if (r.exited && r.exitCode == kToolExitNoProtocol) {
        Log("[Clipboard] typeless-clipboard: compositor lacks ext-data-control, using wl-clipboard");
        g_toolUnusable.store(true);
    }
}

bool ToolSet(const std::vector<std::pair<std::string, std::string>> &entries)
{
    std::string payload;
    for (const auto &e : entries) AppendFrame(payload, e.first, e.second);
    SpawnResult r = RunProcess({ToolPath(), "set"}, payload, 2000, 4096);
    NoteToolResult(r);
    return r.exited && r.exitCode == 0;
}

// Same MIME set wl-copy advertises for text.
std::vector<std::pair<std::string, std::string>> TextEntries(const std::string &text)
{
    return {{kPlainUtf8, text}, {"text/plain", text}, {"UTF8_STRING", text}, {"STRING", text}, {"TEXT", text}};
}

bool WlCopy(const std::string &mime, const std::string &data)
{
    if (!CommandExists("wl-copy")) return false;
    SpawnResult r = RunProcess({"wl-copy", "--type", mime}, data, 2000, 4096);
    return r.exited && r.exitCode == 0;
}

bool WlPaste(const std::string &mime, std::string &out)
{
    if (!CommandExists("wl-paste")) return false;
    SpawnResult r = RunProcess({"wl-paste", "--no-newline", "--type", mime}, "", 1500);
    if (!r.exited || r.exitCode != 0) return false;
    out = r.out;
    return true;
}

}  // namespace

std::string Snapshot::PlainText() const
{
    static const char *const prefs[] = {kPlainUtf8, "text/plain", "UTF8_STRING", "STRING", "TEXT", nullptr};
    for (int p = 0; prefs[p]; p++) {
        for (const auto &e : entries) {
            if (e.first == prefs[p]) return e.second;
        }
    }
    return std::string();
}

Snapshot Save()
{
    Snapshot snap;
    if (ToolAvailable()) {
        SpawnResult r = RunProcess({ToolPath(), "get"}, "", 1500);
        NoteToolResult(r);
        if (r.exited && (r.exitCode == 0 || r.exitCode == kToolExitEmpty) && ParseFrames(r.out, snap.entries)) {
            return snap;
        }
        snap.entries.clear();
    }
    // Fallback: plain text only, like the original.
    std::string text;
    if (WlPaste(kPlainUtf8, text) || WlPaste("text", text)) {
        if (!text.empty()) snap.entries = TextEntries(text);
    }
    return snap;
}

bool Restore(const Snapshot &snapshot)
{
    if (snapshot.Empty()) return Clear();
    if (ToolAvailable() && ToolSet(snapshot.entries)) return true;
    std::string text = snapshot.PlainText();
    if (!text.empty()) return WlCopy(kPlainUtf8, text);
    return Clear();
}

bool SetText(const std::string &text)
{
    if (ToolAvailable() && ToolSet(TextEntries(text))) return true;
    return WlCopy(kPlainUtf8, text);
}

bool SetRichText(const std::string &html, const std::string &text)
{
    if (ToolAvailable()) {
        auto entries = TextEntries(text);
        entries.insert(entries.begin(), {"text/html", html});
        if (ToolSet(entries)) return true;
    }
    // wl-copy can offer one type only. The original offers text/html here; plain
    // text is the safer single choice because every target accepts it.
    return WlCopy(kPlainUtf8, text.empty() ? HtmlToPlainText(html) : text);
}

bool Clear()
{
    if (ToolAvailable()) {
        SpawnResult r = RunProcess({ToolPath(), "clear"}, "", 1500, 4096);
        NoteToolResult(r);
        if (r.exited && r.exitCode == 0) return true;
    }
    if (!CommandExists("wl-copy")) return false;
    SpawnResult r = RunProcess({"wl-copy", "--clear"}, "", 1500, 4096);
    return r.exited && r.exitCode == 0;
}

std::string GetText()
{
    if (ToolAvailable()) {
        SpawnResult r = RunProcess({ToolPath(), "get", "--mime", kPlainUtf8, "--mime", "text/plain", "--mime",
                                    "UTF8_STRING", "--mime", "STRING", "--mime", "TEXT"},
                                   "", 1500);
        NoteToolResult(r);
        std::vector<std::pair<std::string, std::string>> entries;
        if (r.exited && r.exitCode == kToolExitEmpty) return std::string();
        if (r.exited && r.exitCode == 0 && ParseFrames(r.out, entries) && !entries.empty()) {
            return entries.front().second;
        }
    }
    std::string text;
    if (WlPaste(kPlainUtf8, text) || WlPaste("text", text)) return text;
    return std::string();
}

}  // namespace Clipboard
}  // namespace InputHelper
