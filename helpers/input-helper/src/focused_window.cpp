// Focused-window lookup used to pick the paste/copy chord.
// Not in the original (which always sends Ctrl+V / Ctrl+C); added because on
// Hyprland terminals are common targets and Ctrl+V does not paste there while
// Ctrl+C would interrupt the foreground process.
#include "focused_window.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "common.h"

namespace InputHelper {
namespace FocusedWindow {
namespace {

bool HyprlandRequest(const std::string &request, std::string &reply, int timeoutMs)
{
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig || !*sig) return false;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    std::string path;
    if (runtime && *runtime) {
        path = std::string(runtime) + "/hypr/" + sig + "/.socket.sock";
    }
    if (path.empty() || access(path.c_str(), F_OK) != 0) {
        path = std::string("/tmp/hypr/") + sig + "/.socket.sock";  // pre-0.40 location
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) return false;
    memcpy(addr.sun_path, path.c_str(), path.size());

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        send(fd, request.data(), request.size(), MSG_NOSIGNAL) != (ssize_t)request.size()) {
        close(fd);
        return false;
    }
    int64_t deadline = NowMs() + timeoutMs;
    char buf[8192];
    bool ok = false;
    for (;;) {
        int64_t left = deadline - NowMs();
        if (left <= 0) break;
        struct pollfd pfd = {fd, POLLIN, 0};
        int pr = poll(&pfd, 1, (int)left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) break;
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n > 0) {
            reply.append(buf, (size_t)n);
            if (reply.size() > (1u << 20)) break;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        ok = (n == 0);
        break;
    }
    close(fd);
    return ok && !reply.empty();
}

}  // namespace

Info Query(int timeoutMs)
{
    Info info;
    std::string reply;
    if (!HyprlandRequest("j/activewindow", reply, timeoutMs)) return info;
    JsonValue v;
    if (!JsonParse(reply, v) || v.type != JsonValue::Object) {
        // "{}" or plain text when nothing is focused.
        return info;
    }
    info.known = true;
    info.windowClass = v.GetString("class");
    info.initialClass = v.GetString("initialClass");
    info.title = v.GetString("title");
    info.pid = (int)v.GetNumber("pid", 0);
    if (const JsonValue *tags = v.Get("tags")) {
        if (tags->type == JsonValue::Array) {
            for (const auto &t : tags->a) {
                if (t.type == JsonValue::String) info.tags.push_back(t.s);
            }
        }
    }
    return info;
}

bool IsTerminal(const Info &info)
{
    if (!info.known) return false;
    // Omarchy tags terminals (default/hypr/apps/terminals.lua); dynamic tags end in '*'.
    for (std::string tag : info.tags) {
        if (!tag.empty() && tag.back() == '*') tag.pop_back();
        if (tag == "terminal") return true;
    }
    static const char *const classes[] = {
        "Alacritty", "kitty", "foot", "footclient", "org.codeberg.dnkl.foot", "org.codeberg.dnkl.footclient",
        "com.mitchellh.ghostty", "ghostty", "org.wezfurlong.wezterm", "wezterm", "org.gnome.Console", "kgx",
        "org.gnome.Terminal", "gnome-terminal-server", "gnome-terminal", "org.kde.konsole", "konsole",
        "xterm", "XTerm", "UXTerm", "URxvt", "urxvt", "st", "st-256color", "terminator", "tilix",
        "com.gexperts.Tilix", "xfce4-terminal", "Xfce4-terminal", "com.raggesilver.BlackBox", "blackbox",
        "rio", "contour", "org.contourterminal.Contour", "dev.warp.Warp", "sakura", "lxterminal",
        "qterminal", "mate-terminal", "io.elementary.terminal", "Termite", "cool-retro-term",
        "org.kde.yakuake", "yakuake", "Guake", "guake", "tilda", "ptyxis", "org.gnome.Ptyxis",
        "app.devsuite.Ptyxis", "com.system76.CosmicTerm", nullptr};
    for (const std::string *cls : {&info.windowClass, &info.initialClass}) {
        if (cls->empty()) continue;
        for (int i = 0; classes[i]; i++) {
            if (strcasecmp(cls->c_str(), classes[i]) == 0) return true;
        }
        // Omarchy launches TUIs under these app-ids (they are terminals too).
        if (cls->rfind("org.omarchy.", 0) == 0 || cls->rfind("TUI.", 0) == 0) return true;
    }
    return false;
}

}  // namespace FocusedWindow
}  // namespace InputHelper
