#define _GNU_SOURCE
#include "window.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "a11y.h"
#include "json.h"
#include "util.h"

// ---------------------------------------------------------------------------
// Hyprland IPC
// ---------------------------------------------------------------------------

static bool hypr_socket_path(char *out, size_t cap) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char rtbuf[64];
    if (!rt || !*rt) {
        snprintf(rtbuf, sizeof rtbuf, "/run/user/%d", (int)getuid());
        rt = rtbuf;
    }
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    struct stat st;
    if (sig && *sig) {
        snprintf(out, cap, "%s/hypr/%s/.socket.sock", rt, sig);
        if (stat(out, &st) == 0 && S_ISSOCK(st.st_mode)) return true;
    }
    // Signature not in our environment: pick the most recently started instance.
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/hypr", rt);
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *de;
    time_t best = 0;
    bool found = false;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char cand[PATH_MAX + 512];
        snprintf(cand, sizeof cand, "%s/%s/.socket.sock", dir, de->d_name);
        size_t cl = strlen(cand);
        if (cl < cap && stat(cand, &st) == 0 && S_ISSOCK(st.st_mode) && (!found || st.st_mtime > best)) {
            best = st.st_mtime;
            memcpy(out, cand, cl + 1);
            found = true;
        }
    }
    closedir(d);
    return found;
}

// Sends one request, returns malloc()ed reply (NUL-terminated) or NULL.
static char *hypr_request(const char *req, int timeout_ms, size_t *out_len) {
    char path[PATH_MAX];
    if (!hypr_socket_path(path, sizeof path)) return NULL;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path) return NULL;
    strcpy(sa.sun_path, path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return NULL;
    int64_t deadline = now_ms() + timeout_ms;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) goto fail;
        struct pollfd pfd = {fd, POLLOUT, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) goto fail;
        int err = 0;
        socklen_t el = sizeof err;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) goto fail;
    }
    size_t rl = strlen(req), off = 0;
    while (off < rl) {
        ssize_t w = send(fd, req + off, rl - off, MSG_NOSIGNAL);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            int left = (int)(deadline - now_ms());
            struct pollfd pfd = {fd, POLLOUT, 0};
            if (left <= 0 || poll(&pfd, 1, left) <= 0) goto fail;
            continue;
        }
        goto fail;
    }
    sb_t b;
    sb_init(&b);
    for (;;) {
        char buf[8192];
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r > 0) {
            sb_putn(&b, buf, (size_t)r);
            if (b.n > (4u << 20)) break; // absurdly large; stop
            continue;
        }
        if (r == 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN) {
            int left = (int)(deadline - now_ms());
            struct pollfd pfd = {fd, POLLIN, 0};
            if (left <= 0 || poll(&pfd, 1, left) <= 0) {
                sb_free(&b);
                goto fail;
            }
            continue;
        }
        sb_free(&b);
        goto fail;
    }
    close(fd);
    if (out_len) *out_len = b.n;
    return sb_take(&b);
fail:
    close(fd);
    return NULL;
}

int hypr_active_window(fg_window_t *w, int timeout_ms) {
    memset(w, 0, sizeof *w);
    // Test hook: pretend another window is focused (lets the test harness
    // inspect a background app without stealing the user's focus).
    const char *tp = getenv("TYPELESS_CONTEXT_HELPER_TEST_PID");
    if (tp && atoi(tp) > 0) {
        const char *tt = getenv("TYPELESS_CONTEXT_HELPER_TEST_TITLE");
        w->pid = atoi(tp);
        w->title = xstrdup(tt);
        w->cls = xstrdup("test");
        w->visible = true;
        return 1;
    }
    size_t len = 0;
    char *reply = hypr_request("j/activewindow", timeout_ms, &len);
    if (!reply) return -1;
    jv *v = jv_parse(reply, len);
    free(reply);
    if (!v || v->t != JV_OBJ) {
        jv_free(v);
        return -1;
    }
    if (v->n == 0) { // "{}" = no focused window
        jv_free(v);
        return 0;
    }
    w->pid = (int)jv_num(jv_get(v, "pid"), 0);
    w->title = xstrdup(jv_str(jv_get(v, "title"), ""));
    w->cls = xstrdup(jv_str(jv_get(v, "class"), ""));
    const jv *at = jv_get(v, "at"), *size = jv_get(v, "size");
    w->x = (int)jv_num(jv_at(at, 0), 0);
    w->y = (int)jv_num(jv_at(at, 1), 0);
    w->w = (int)jv_num(jv_at(size, 0), 0);
    w->h = (int)jv_num(jv_at(size, 1), 0);
    w->visible = jv_bool(jv_get(v, "mapped"), true) && !jv_bool(jv_get(v, "hidden"), false);
    w->xwayland = jv_bool(jv_get(v, "xwayland"), false);
    jv_free(v);
    return 1;
}

void fg_window_free(fg_window_t *w) {
    free(w->title);
    free(w->cls);
    memset(w, 0, sizeof *w);
}

// ---------------------------------------------------------------------------
// /proc helpers
// ---------------------------------------------------------------------------

char *path_basename(const char *path) {
    if (!path) return xstrdup("");
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    size_t s = n;
    while (s > 0 && path[s - 1] != '/') s--;
    return xstrndup(path + s, n - s);
}

char *proc_exe_path(int pid) {
    if (pid <= 0) return xstrdup("");
    char p[64], buf[PATH_MAX];
    snprintf(p, sizeof p, "/proc/%d/exe", pid);
    ssize_t r = readlink(p, buf, sizeof buf - 1);
    if (r <= 0) return xstrdup("");
    buf[r] = 0;
    if (str_has_suffix(buf, " (deleted)")) buf[r - 10] = 0;
    return xstrdup(buf);
}

char *proc_fallback_name(int pid) {
    if (pid <= 0) return xstrdup("");
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
    FILE *f = fopen(p, "re");
    if (f) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        if (n > 0 && buf[0]) {
            char *b = path_basename(buf); // argv[0] only (NUL separated)
            if (b[0]) return b;
            free(b);
        }
    }
    snprintf(p, sizeof p, "/proc/%d/comm", pid);
    f = fopen(p, "re");
    if (f) {
        char buf[256];
        if (fgets(buf, sizeof buf, f)) {
            fclose(f);
            buf[strcspn(buf, "\n")] = 0;
            return xstrdup(buf);
        }
        fclose(f);
    }
    return xstrdup("");
}

// ---------------------------------------------------------------------------
// Browser heuristics (ContextHelper::BrowserUtils / BrowserHelper equivalents)
// ---------------------------------------------------------------------------

static bool is_browser_app(const char *bundle_id) {
    // Original: lower-cased substring match against this list.
    static const char *const k_substr[] = {
        "chrome", "chromium", "firefox", "msedge", "opera", "brave", "vivaldi", "waterfox",
        "tor browser", "librewolf", "epiphany", "surf", "falkon", "midori",
    };
    // Additions for browsers common on Arch/Omarchy that the x64 build misses
    // (exact basename match to avoid false positives such as "zenity").
    static const char *const k_exact[] = {
        "helium", "zen", "zen-bin", "zen-browser", "floorp", "thorium", "thorium-browser", "cromite",
        "mullvadbrowser", "qutebrowser",
    };
    char *l = xstrdup(bundle_id);
    str_lower_ascii(l);
    bool hit = false;
    for (size_t i = 0; !hit && i < sizeof k_substr / sizeof *k_substr; i++)
        if (strstr(l, k_substr[i])) hit = true;
    for (size_t i = 0; !hit && i < sizeof k_exact / sizeof *k_exact; i++)
        if (!strcmp(l, k_exact[i])) hit = true;
    free(l);
    return hit;
}

static char *extract_page_title(const char *window_title) {
    static const char *const k_suffixes[] = {
        " - Google Chrome", " - Chrome", " - Chromium", " - Microsoft Edge", " - Edge",
        " - Mozilla Firefox", " - Firefox", " - Safari", " - Opera", " - Brave", " - Vivaldi",
        // additions
        " — Mozilla Firefox", " — Zen Browser", " - Zen Browser", " — Zen", " - Helium", " — LibreWolf",
        " - LibreWolf", " — Floorp", " - Thorium",
    };
    char *t = xstrdup(window_title);
    for (size_t i = 0; i < sizeof k_suffixes / sizeof *k_suffixes; i++) {
        if (str_has_suffix(t, k_suffixes[i])) {
            t[strlen(t) - strlen(k_suffixes[i])] = 0;
            break;
        }
    }
    return t;
}

// ---------------------------------------------------------------------------
// getFocusedAppInfo JSON
// ---------------------------------------------------------------------------

char *build_app_info_json(void) {
    int64_t t0 = now_ms();
    fg_window_t w;
    int r = hypr_active_window(&w, 300);
    if (r < 0) {
        // Not on Hyprland (or IPC broken): fall back to the AT-SPI active window.
        if (!a11y_active_window(&w, 300)) r = 0;
        else r = 1;
    }
    if (r <= 0 || w.pid <= 0) {
        // Original returns "{}" when there is neither a pid nor an app name.
        fg_window_free(&w);
        return xstrdup("{}");
    }

    char *exe = proc_exe_path(w.pid);
    char *app = exe[0] ? path_basename(exe) : xstrdup("");
    if (!app[0]) {
        free(app);
        app = proc_fallback_name(w.pid);
    }
    if (!app[0] && w.cls && w.cls[0]) {
        free(app);
        app = xstrdup(w.cls);
    }
    const char *bundle = app; // original: bundleId == appName (exe basename)
    bool browser = is_browser_app(bundle);

    char *web_url = NULL, *web_title = NULL;
    if (browser && w.pid != getpid()) {
        web_url = a11y_browser_url(w.pid, w.title, 250);
        web_title = extract_page_title(w.title);
    }

    sb_t b;
    sb_init(&b);
    sb_putc(&b, '{');
    sb_json_kv_str(&b, "appName", app);
    sb_printf(&b, ",\"processId\":%d,", w.pid);
    sb_json_kv_str(&b, "bundleId", bundle);
    sb_putc(&b, ',');
    sb_json_kv_str(&b, "executablePath", exe);
    sb_putc(&b, ',');
    sb_json_kv_str(&b, "bundlePath", exe);
    sb_putc(&b, ',');
    sb_json_kv_str(&b, "windowTitle", w.title);
    sb_puts(&b, ",\"windowId\":0,");
    sb_printf(&b, "\"windowBounds\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d},", w.x, w.y, w.w, w.h);
    sb_printf(&b, "\"isWebBrowser\":%s,\"isVisible\":%s,", browser ? "true" : "false", w.visible ? "true" : "false");
    sb_json_kv_str(&b, "className", w.cls);
    if (browser) {
        sb_putc(&b, ',');
        sb_json_kv_str(&b, "webURL", web_url ? web_url : "");
        sb_putc(&b, ',');
        sb_json_kv_str(&b, "webTitle", web_title ? web_title : "");
    }
    sb_putc(&b, '}');
    dbg("appinfo in %lldms: %s", (long long)(now_ms() - t0), b.p ? b.p : "");
    free(web_url);
    free(web_title);
    free(exe);
    free(app);
    fg_window_free(&w);
    char *out = sb_take(&b);
    return out ? out : xstrdup("{}");
}
