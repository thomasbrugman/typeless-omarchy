// Clean-room AT-SPI client for libContextHelper.
//
// The x64 build spawns a separate `typeless-atspi-helper` process that links
// libatspi/GLib and answers JSON requests over a unix socket. Here we talk the
// AT-SPI D-Bus protocol directly with libdbus on *private* connections, which
// needs no GLib main loop and cannot interfere with Electron's own D-Bus/ATK
// usage. All calls carry timeouts; nothing ever calls into our own process.
#define _GNU_SOURCE
#include "a11y.h"

#include <dbus/dbus.h>
#include <dlfcn.h>
#include <poll.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

#define REGISTRY_BUS "org.a11y.atspi.Registry"
#define PATH_ROOT "/org/a11y/atspi/accessible/root"
#define PATH_NULL "/org/a11y/atspi/null"
#define IF_ACC "org.a11y.atspi.Accessible"
#define IF_TEXT "org.a11y.atspi.Text"
#define IF_COMPONENT "org.a11y.atspi.Component"
#define IF_DOCUMENT "org.a11y.atspi.Document"
#define IF_EDITABLE "org.a11y.atspi.EditableText"
#define IF_PROPS "org.freedesktop.DBus.Properties"

// AtspiStateType
enum {
    ST_ACTIVE = 1, ST_DEFUNCT = 6, ST_EDITABLE = 7, ST_ENABLED = 8, ST_FOCUSED = 12,
    ST_MULTI_LINE = 17, ST_SHOWING = 25, ST_VISIBLE = 30, ST_MANAGES_DESCENDANTS = 31,
    ST_READ_ONLY = 43,
};
// AtspiRole (subset)
enum {
    R_FRAME = 23, R_LABEL = 29, R_LIST_ITEM = 32, R_PASSWORD_TEXT = 40, R_TERMINAL = 60, R_TEXT = 61,
    R_WINDOW = 69, R_APPLICATION = 75, R_EDITBAR = 77, R_ENTRY = 79, R_DOCUMENT_FRAME = 82, R_TREE_ITEM = 91,
    R_DOCUMENT_TEXT = 94, R_DOCUMENT_WEB = 95,
};
enum { IFB_TEXT = 1, IFB_EDITABLE = 2, IFB_DOCUMENT = 4, IFB_COMPONENT = 8, IFB_HYPERTEXT = 16 };

// Role names as returned by atspi_role_get_name() in at-spi2-core 2.44 (what
// the x64 helper was built against; newer libatspi renamed 43 to "button").
static const char *const k_role_names[] = {
    "invalid", "accelerator label", "alert", "animation", "arrow", "calendar", "canvas", "check box",
    "check menu item", "color chooser", "column header", "combo box", "date editor", "desktop icon",
    "desktop frame", "dial", "dialog", "directory pane", "drawing area", "file chooser", "filler",
    "focus traversable", "font chooser", "frame", "glass pane", "html container", "icon", "image",
    "internal frame", "label", "layered pane", "list", "list item", "menu", "menu bar", "menu item",
    "option pane", "page tab", "page tab list", "panel", "password text", "popup menu", "progress bar",
    "push button", "radio button", "radio menu item", "root pane", "row header", "scroll bar",
    "scroll pane", "separator", "slider", "spin button", "split pane", "status bar", "table",
    "table cell", "table column header", "table row header", "tearoff menu item", "terminal", "text",
    "toggle button", "tool bar", "tool tip", "tree", "tree table", "unknown", "viewport", "window",
    "extended", "header", "footer", "paragraph", "ruler", "application", "autocomplete", "editbar",
    "embedded", "entry", "chart", "caption", "document frame", "heading", "page", "section",
    "redundant object", "form", "link", "input method window", "table row", "tree item",
    "document spreadsheet", "document presentation", "document text", "document web",
    "document email", "comment", "list box", "grouping", "image map", "notification", "info bar",
    "level bar", "title bar", "block quote", "audio", "video", "definition", "article", "landmark",
    "log", "marquee", "math", "rating", "timer", "static", "math fraction", "math root", "subscript",
    "superscript", "description list", "description term", "description value", "footnote",
    "content deletion", "content insertion", "mark", "suggestion", "push button menu", "switch",
};

typedef struct {
    char *bus;
    char *path;
} aref;

static void aref_free(aref *r) {
    free(r->bus);
    free(r->path);
    r->bus = r->path = NULL;
}

static bool aref_valid(const aref *r) {
    return r->bus && r->path && r->bus[0] && strcmp(r->path, PATH_NULL) != 0 &&
           dbus_validate_bus_name(r->bus, NULL) && dbus_validate_path(r->path, NULL);
}

static bool st_has(const uint32_t st[2], int bit) { return bit < 64 && (st[bit / 32] >> (bit % 32)) & 1u; }

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

// Accessibility-bus connection with explicit user counting: libdbus is not
// safe against dbus_connection_close() racing other threads that are still
// using the connection, so a retired connection is only closed once the last
// thread using it has released it (conn_put).
typedef struct aconn {
    DBusConnection *c;
    int users;
    struct aconn *next;
} aconn;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;       // connection lifecycle
static aconn *g_cur;                                            // live connection
static aconn *g_retired;                                        // retired, still in use
static int64_t g_next_attempt;
static atomic_int g_reset_req;       // recover_after_resume: drop connection at next use
static atomic_int g_bus_unreachable; // last connection attempt failed
static atomic_int g_last_enable;     // 0 unknown, 1 ok, -1 failed
static bool g_status_attempted;

static pthread_mutex_t g_sess_mu = PTHREAD_MUTEX_INITIALIZER;  // session bus
static DBusConnection *g_sess;

static pthread_mutex_t g_ev_mu = PTHREAD_MUTEX_INITIALIZER;    // event caches
static aref g_focus;          // last object that reported state-changed:focused=1
static int64_t g_focus_t;
static aref g_active_win;     // last window:activate
static int64_t g_active_win_t;

static pthread_mutex_t g_evt_mu = PTHREAD_MUTEX_INITIALIZER;   // event thread lifecycle
static pthread_t g_evt;
static bool g_evt_running;
static volatile int g_evt_stop;

// pid cache for unique bus names (unique names are never reused).
#define PID_CACHE 256
static pthread_mutex_t g_pid_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { char name[64]; int pid; } g_pids[PID_CACHE];
static int g_npids, g_pid_next;

static void events_thread_start(void);

static void set_str(char **dst, const char *src) {
    free(*dst);
    *dst = src ? strdup(src) : NULL;
}

static DBusHandlerResult ev_filter(DBusConnection *c, DBusMessage *m, void *ud) {
    (void)c; (void)ud;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_SIGNAL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *iface = dbus_message_get_interface(m), *member = dbus_message_get_member(m);
    const char *sender = dbus_message_get_sender(m), *path = dbus_message_get_path(m);
    if (!iface || !member || !sender || !path) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (!strcmp(iface, "org.a11y.atspi.Event.Object") && !strcmp(member, "StateChanged")) {
        DBusMessageIter it;
        if (!dbus_message_iter_init(m, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        const char *kind = NULL;
        dbus_message_iter_get_basic(&it, &kind);
        if (!kind || strcmp(kind, "focused") != 0 || !dbus_message_iter_next(&it) ||
            dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INT32)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        dbus_int32_t d1 = 0;
        dbus_message_iter_get_basic(&it, &d1);
        pthread_mutex_lock(&g_ev_mu);
        if (d1) {
            set_str(&g_focus.bus, sender);
            set_str(&g_focus.path, path);
            g_focus_t = now_ms();
        } else if (str_eq(g_focus.bus, sender) && str_eq(g_focus.path, path)) {
            aref_free(&g_focus);
        }
        pthread_mutex_unlock(&g_ev_mu);
    } else if (!strcmp(iface, "org.a11y.atspi.Event.Focus") && !strcmp(member, "Focus")) {
        pthread_mutex_lock(&g_ev_mu);
        set_str(&g_focus.bus, sender);
        set_str(&g_focus.path, path);
        g_focus_t = now_ms();
        pthread_mutex_unlock(&g_ev_mu);
    } else if (!strcmp(iface, "org.a11y.atspi.Event.Window")) {
        pthread_mutex_lock(&g_ev_mu);
        if (!strcmp(member, "Activate")) {
            set_str(&g_active_win.bus, sender);
            set_str(&g_active_win.path, path);
            g_active_win_t = now_ms();
        } else if (!strcmp(member, "Deactivate") && str_eq(g_active_win.bus, sender) &&
                   str_eq(g_active_win.path, path)) {
            aref_free(&g_active_win);
        }
        pthread_mutex_unlock(&g_ev_mu);
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusConnection *session_get_locked(void) {
    dbus_threads_init_default();
    if (g_sess && !dbus_connection_get_is_connected(g_sess)) {
        dbus_connection_close(g_sess);
        dbus_connection_unref(g_sess);
        g_sess = NULL;
    }
    if (!g_sess) {
        DBusError err;
        dbus_error_init(&err);
        g_sess = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
        if (dbus_error_is_set(&err)) {
            dbg("session bus: %s", err.message);
            dbus_error_free(&err);
        }
        if (g_sess) dbus_connection_set_exit_on_disconnect(g_sess, FALSE);
    }
    return g_sess;
}

static DBusMessage *call_block(DBusConnection *c, DBusMessage *m, int timeout_ms) {
    if (!c || !m) {
        if (m) dbus_message_unref(m);
        return NULL;
    }
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, timeout_ms > 0 ? timeout_ms : 1, &err);
    dbus_message_unref(m);
    if (dbus_error_is_set(&err)) {
        dbg("call failed: %s: %s", err.name, err.message);
        dbus_error_free(&err);
    }
    return r;
}

static char *get_a11y_bus_address(void) {
    const char *env = getenv("AT_SPI_BUS_ADDRESS");
    if (env && *env) return strdup(env);
    char *addr = NULL;
    pthread_mutex_lock(&g_sess_mu);
    DBusConnection *s = session_get_locked();
    if (s) {
        DBusMessage *m = dbus_message_new_method_call("org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress");
        DBusMessage *r = call_block(s, m, 1000);
        const char *a = NULL;
        if (r && dbus_message_get_args(r, NULL, DBUS_TYPE_STRING, &a, DBUS_TYPE_INVALID) && a && *a) addr = strdup(a);
        if (r) dbus_message_unref(r);
    }
    pthread_mutex_unlock(&g_sess_mu);
    return addr;
}

static void register_event(DBusConnection *c, const char *ev) {
    // Newer registryd: RegisterEvent(s event, as properties, s app_bus_name);
    // older ones take just (s). Try the new signature first.
    DBusMessage *m = dbus_message_new_method_call(REGISTRY_BUS, "/org/a11y/atspi/registry", "org.a11y.atspi.Registry",
                                                  "RegisterEvent");
    if (!m) return;
    DBusMessageIter it, arr;
    const char *empty = "";
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &ev);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
    dbus_message_iter_close_container(&it, &arr);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &empty);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, 500, &err);
    dbus_message_unref(m);
    if (r) {
        dbus_message_unref(r);
        return;
    }
    bool retry = dbus_error_is_set(&err) && err.name && strstr(err.name, "Timeout") == NULL;
    dbus_error_free(&err);
    if (!retry) return;
    m = dbus_message_new_method_call(REGISTRY_BUS, "/org/a11y/atspi/registry", "org.a11y.atspi.Registry",
                                     "RegisterEvent");
    if (!m) return;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &ev, DBUS_TYPE_INVALID);
    r = call_block(c, m, 500);
    if (r) dbus_message_unref(r);
}

static void ac_destroy(aconn *a) {
    dbus_connection_remove_filter(a->c, ev_filter, NULL);
    dbus_connection_close(a->c);
    dbus_connection_unref(a->c);
    free(a);
}

static void conn_drop_locked(void) {
    aconn *a = g_cur;
    if (!a) return;
    g_cur = NULL;
    if (a->users == 0) {
        ac_destroy(a);
    } else {
        a->next = g_retired;
        g_retired = a;
    }
}

// Releases a connection obtained from conn_get()/conn_peek().
static void conn_put(DBusConnection *c) {
    if (!c) return;
    pthread_mutex_lock(&g_mu);
    if (g_cur && g_cur->c == c) {
        g_cur->users--;
    } else {
        for (aconn **pp = &g_retired; *pp; pp = &(*pp)->next) {
            if ((*pp)->c == c) {
                aconn *a = *pp;
                if (--a->users == 0) {
                    *pp = a->next;
                    ac_destroy(a);
                }
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
}

// Current connection without attempting to connect (NULL if none).
static DBusConnection *conn_peek(void) {
    pthread_mutex_lock(&g_mu);
    DBusConnection *c = NULL;
    if (g_cur && !atomic_load(&g_reset_req)) {
        g_cur->users++;
        c = g_cur->c;
    }
    pthread_mutex_unlock(&g_mu);
    return c;
}

// Returns a new reference to the accessibility-bus connection, or NULL.
static DBusConnection *conn_get(void) {
    bool fresh = false;
    pthread_mutex_lock(&g_mu);
    if (atomic_exchange(&g_reset_req, 0)) {
        conn_drop_locked();
        g_next_attempt = 0;
        g_status_attempted = false;
    }
    if (g_cur && !dbus_connection_get_is_connected(g_cur->c)) conn_drop_locked();
    if (!g_cur && now_ms() >= g_next_attempt) {
        dbus_threads_init_default();
        char *addr = get_a11y_bus_address();
        if (addr) {
            DBusError err;
            dbus_error_init(&err);
            DBusConnection *c = dbus_connection_open_private(addr, &err);
            if (c && dbus_bus_register(c, &err)) {
                dbus_connection_set_exit_on_disconnect(c, FALSE);
                dbus_connection_add_filter(c, ev_filter, NULL, NULL);
                static const char *const rules[] = {
                    "type='signal',interface='org.a11y.atspi.Event.Object',member='StateChanged',arg0='focused'",
                    "type='signal',interface='org.a11y.atspi.Event.Focus'",
                    "type='signal',interface='org.a11y.atspi.Event.Window',member='Activate'",
                    "type='signal',interface='org.a11y.atspi.Event.Window',member='Deactivate'",
                };
                for (size_t i = 0; i < sizeof rules / sizeof *rules; i++) dbus_bus_add_match(c, rules[i], NULL);
                static const char *const events[] = {"object:state-changed:focused", "focus:", "window:activate",
                                                     "window:deactivate"};
                for (size_t i = 0; i < sizeof events / sizeof *events; i++) register_event(c, events[i]);
                g_cur = calloc(1, sizeof *g_cur);
                if (g_cur) {
                    g_cur->c = c;
                    fresh = true;
                } else {
                    dbus_connection_remove_filter(c, ev_filter, NULL);
                    dbus_connection_close(c);
                    dbus_connection_unref(c);
                }
                dbg("connected to a11y bus %s", addr);
            } else {
                if (dbus_error_is_set(&err)) dbg("a11y bus connect failed: %s", err.message);
                if (c) {
                    dbus_connection_close(c);
                    dbus_connection_unref(c);
                }
            }
            dbus_error_free(&err);
            free(addr);
        }
        if (!g_cur) g_next_attempt = now_ms() + 3000;
        atomic_store(&g_bus_unreachable, g_cur ? 0 : 1);
    }
    DBusConnection *c = NULL;
    if (g_cur) {
        g_cur->users++;
        c = g_cur->c;
    }
    bool do_status = fresh && !g_status_attempted;
    if (do_status) g_status_attempted = true;
    pthread_mutex_unlock(&g_mu);
    if (c) events_thread_start();
    if (do_status) a11y_enable_session(false);
    return c;
}

// Drain queued signals so the focus cache is current.
static void pump_events(DBusConnection *c) {
    if (!c) return;
    dbus_connection_read_write(c, 0);
    while (dbus_connection_dispatch(c) == DBUS_DISPATCH_DATA_REMAINS) {}
}

static void *events_main(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "ctxh-a11y-ev");
    while (!g_evt_stop) {
        DBusConnection *c = conn_peek();
        if (!c) {
            struct timespec ts = {0, 200 * 1000000L};
            nanosleep(&ts, NULL);
            continue;
        }
        int fd = -1;
        if (!dbus_connection_get_unix_fd(c, &fd) || fd < 0 || !dbus_connection_get_is_connected(c)) {
            conn_put(c);
            struct timespec ts = {0, 200 * 1000000L};
            nanosleep(&ts, NULL);
            continue;
        }
        struct pollfd pfd = {fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 250);
        if (pr > 0) {
            // Non-blocking: never hold libdbus' I/O path while waiting, so other
            // threads' blocking calls are not delayed.
            dbus_connection_read_write(c, 0);
        }
        bool any = false;
        while (dbus_connection_dispatch(c) == DBUS_DISPATCH_DATA_REMAINS) any = true;
        if (pr > 0 && !any) {
            // Another thread is consuming the socket; avoid a busy loop.
            struct timespec ts = {0, 3 * 1000000L};
            nanosleep(&ts, NULL);
        }
        conn_put(c);
    }
    return NULL;
}

static void events_thread_start(void) {
    pthread_mutex_lock(&g_evt_mu);
    if (!g_evt_running) {
        g_evt_stop = 0;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 256 * 1024);
        if (pthread_create(&g_evt, &a, events_main, NULL) == 0) g_evt_running = true;
        pthread_attr_destroy(&a);
    }
    pthread_mutex_unlock(&g_evt_mu);
}

static void pid_cache_clear(void) {
    pthread_mutex_lock(&g_pid_mu);
    g_npids = 0;
    g_pid_next = 0;
    pthread_mutex_unlock(&g_pid_mu);
}

static void url_cache_clear(void);

// Never blocks on a connection attempt in progress (called on the JS thread):
// the connection is dropped by the next conn_get().
void a11y_recover_after_resume(void) {
    atomic_store(&g_reset_req, 1);
    atomic_store(&g_bus_unreachable, 0);
    pthread_mutex_lock(&g_ev_mu);
    aref_free(&g_focus);
    aref_free(&g_active_win);
    pthread_mutex_unlock(&g_ev_mu);
    pid_cache_clear();
    url_cache_clear();
}

void a11y_shutdown(void) {
    pthread_mutex_lock(&g_evt_mu);
    if (g_evt_running) {
        g_evt_stop = 1;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        if (pthread_timedjoin_np(g_evt, NULL, &ts) != 0) pthread_detach(g_evt);
        g_evt_running = false;
    }
    pthread_mutex_unlock(&g_evt_mu);
    a11y_recover_after_resume();
    pthread_mutex_lock(&g_mu);
    atomic_store(&g_reset_req, 0);
    conn_drop_locked();
    g_next_attempt = 0;
    g_status_attempted = false;
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_sess_mu);
    if (g_sess) {
        dbus_connection_close(g_sess);
        dbus_connection_unref(g_sess);
        g_sess = NULL;
    }
    pthread_mutex_unlock(&g_sess_mu);
}

// ---------------------------------------------------------------------------
// Session accessibility status
// ---------------------------------------------------------------------------

static bool set_status_bool(DBusConnection *s, const char *prop) {
    DBusMessage *m = dbus_message_new_method_call("org.a11y.Bus", "/org/a11y/bus", IF_PROPS, "Set");
    if (!m) return false;
    const char *iface = "org.a11y.Status";
    dbus_bool_t v = TRUE;
    DBusMessageIter it, var;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
    dbus_message_iter_close_container(&it, &var);
    DBusMessage *r = call_block(s, m, 300);
    if (!r) return false;
    dbus_message_unref(r);
    return true;
}

// gsettings org.gnome.desktop.interface toolkit-accessibility=true, via a
// runtime-resolved GIO (already loaded in Electron) so we do not link GLib.
static void set_toolkit_accessibility(void) {
    typedef void *(*src_default_t)(void);
    typedef void *(*src_lookup_t)(void *, const char *, int);
    typedef int (*has_key_t)(void *, const char *);
    typedef void (*unref_t)(void *);
    typedef void *(*new_full_t)(void *, void *, const char *);
    typedef int (*get_bool_t)(void *, const char *);
    typedef int (*set_bool_t)(void *, const char *, int);
    typedef void (*sync_t)(void);
    void *h = dlopen("libgio-2.0.so.0", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    if (!h) h = dlopen("libgio-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) return;
    src_default_t src_default = (src_default_t)dlsym(h, "g_settings_schema_source_get_default");
    src_lookup_t src_lookup = (src_lookup_t)dlsym(h, "g_settings_schema_source_lookup");
    has_key_t has_key = (has_key_t)dlsym(h, "g_settings_schema_has_key");
    unref_t schema_unref = (unref_t)dlsym(h, "g_settings_schema_unref");
    new_full_t new_full = (new_full_t)dlsym(h, "g_settings_new_full");
    get_bool_t get_bool = (get_bool_t)dlsym(h, "g_settings_get_boolean");
    set_bool_t set_bool = (set_bool_t)dlsym(h, "g_settings_set_boolean");
    sync_t sync = (sync_t)dlsym(h, "g_settings_sync");
    unref_t obj_unref = (unref_t)dlsym(h, "g_object_unref");
    if (src_default && src_lookup && has_key && schema_unref && new_full && get_bool && set_bool && sync &&
        obj_unref) {
        void *src = src_default();
        void *schema = src ? src_lookup(src, "org.gnome.desktop.interface", 1) : NULL;
        if (schema) {
            if (has_key(schema, "toolkit-accessibility")) {
                void *settings = new_full(schema, NULL, NULL);
                if (settings) {
                    if (!get_bool(settings, "toolkit-accessibility")) {
                        set_bool(settings, "toolkit-accessibility", 1);
                        sync();
                    }
                    obj_unref(settings);
                }
            }
            schema_unref(schema);
        }
    }
    dlclose(h);
}

bool a11y_enable_session(bool reassert) {
    static int64_t last_ok;
    static bool gsettings_done;
    pthread_mutex_lock(&g_sess_mu);
    int64_t t = now_ms();
    bool ok;
    if (last_ok && (!reassert || t - last_ok < 3000)) {
        ok = true; // recently (re)asserted; keep this call cheap
    } else {
        DBusConnection *s = session_get_locked();
        ok = s && set_status_bool(s, "IsEnabled");
        if (s) ok = set_status_bool(s, "ScreenReaderEnabled") && ok;
        if (ok) last_ok = t;
        atomic_store(&g_last_enable, ok ? 1 : -1);
    }
    bool do_gs = !gsettings_done;
    gsettings_done = true;
    pthread_mutex_unlock(&g_sess_mu);
    if (do_gs && !getenv("TYPELESS_CONTEXT_HELPER_NO_GSETTINGS")) set_toolkit_accessibility();
    dbg("enable session a11y (reassert=%d) -> %d", reassert, ok);
    return ok;
}

bool a11y_connected(void) {
    if (pthread_mutex_trylock(&g_mu) != 0) return false; // connecting right now
    bool ok = g_cur && dbus_connection_get_is_connected(g_cur->c) && !atomic_load(&g_reset_req);
    pthread_mutex_unlock(&g_mu);
    return ok;
}

int a11y_enable_status_code(void) {
    if (atomic_load(&g_bus_unreachable)) return -1;
    return atomic_load(&g_last_enable) < 0 ? -2 : 1;
}

bool a11y_desktop_reachable(void) {
    DBusConnection *c = conn_get();
    if (!c) return false;
    conn_put(c);
    return true;
}

// ---------------------------------------------------------------------------
// Message helpers
// ---------------------------------------------------------------------------

static DBusMessage *m_call(const aref *r, const char *iface, const char *member) {
    if (!aref_valid(r)) return NULL;
    return dbus_message_new_method_call(r->bus, r->path, iface, member);
}

static DBusMessage *m_prop(const aref *r, const char *iface, const char *prop) {
    DBusMessage *m = m_call(r, IF_PROPS, "Get");
    if (m) dbus_message_append_args(m, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    return m;
}

typedef struct {
    DBusMessage *m;
    DBusPendingCall *pc;
    DBusMessage *r;
} bcall;

// Sends all calls concurrently, then collects replies (error replies -> NULL).
static void batch_exec(DBusConnection *c, bcall *v, int n, int64_t deadline) {
    const int chunk = 96;
    for (int base = 0; base < n; base += chunk) {
        int end = base + chunk < n ? base + chunk : n;
        for (int i = base; i < end; i++) {
            v[i].pc = NULL;
            v[i].r = NULL;
            if (!v[i].m) continue;
            int left = (int)(deadline - now_ms());
            if (left > 0 && !dbus_connection_send_with_reply(c, v[i].m, &v[i].pc, left)) v[i].pc = NULL;
            dbus_message_unref(v[i].m);
            v[i].m = NULL;
        }
        dbus_connection_flush(c);
        for (int i = base; i < end; i++) {
            if (!v[i].pc) continue;
            dbus_pending_call_block(v[i].pc);
            DBusMessage *r = dbus_pending_call_steal_reply(v[i].pc);
            dbus_pending_call_unref(v[i].pc);
            v[i].pc = NULL;
            if (r && dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
                dbg("batch error: %s", dbus_message_get_error_name(r));
                dbus_message_unref(r);
                r = NULL;
            }
            v[i].r = r;
        }
    }
}

static void batch_free(bcall *v, int n) {
    for (int i = 0; i < n; i++) {
        if (v[i].m) dbus_message_unref(v[i].m);
        if (v[i].r) dbus_message_unref(v[i].r);
    }
    free(v);
}

// Reply parsers -------------------------------------------------------------

static bool iter_first(DBusMessage *r, DBusMessageIter *it) { return r && dbus_message_iter_init(r, it); }

// Unwraps a variant if the iterator points at one.
static void iter_unvariant(DBusMessageIter *it, DBusMessageIter *out) {
    if (dbus_message_iter_get_arg_type(it) == DBUS_TYPE_VARIANT) dbus_message_iter_recurse(it, out);
    else *out = *it;
}

static char *rep_string(DBusMessage *r) {
    DBusMessageIter it, v;
    if (!iter_first(r, &it)) return NULL;
    iter_unvariant(&it, &v);
    int t = dbus_message_iter_get_arg_type(&v);
    if (t != DBUS_TYPE_STRING && t != DBUS_TYPE_OBJECT_PATH) return NULL;
    const char *s = NULL;
    dbus_message_iter_get_basic(&v, &s);
    return s ? strdup(s) : NULL;
}

static bool rep_int(DBusMessage *r, int64_t *out) {
    DBusMessageIter it, v;
    if (!iter_first(r, &it)) return false;
    iter_unvariant(&it, &v);
    switch (dbus_message_iter_get_arg_type(&v)) {
    case DBUS_TYPE_INT32: { dbus_int32_t x; dbus_message_iter_get_basic(&v, &x); *out = x; return true; }
    case DBUS_TYPE_UINT32: { dbus_uint32_t x; dbus_message_iter_get_basic(&v, &x); *out = x; return true; }
    case DBUS_TYPE_INT64: { dbus_int64_t x; dbus_message_iter_get_basic(&v, &x); *out = x; return true; }
    case DBUS_TYPE_INT16: { dbus_int16_t x; dbus_message_iter_get_basic(&v, &x); *out = x; return true; }
    }
    return false;
}

static bool rep_state(DBusMessage *r, uint32_t st[2]) {
    DBusMessageIter it, a;
    st[0] = st[1] = 0;
    if (!iter_first(r, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY ||
        dbus_message_iter_get_element_type(&it) != DBUS_TYPE_UINT32)
        return false;
    dbus_message_iter_recurse(&it, &a);
    for (int i = 0; i < 2 && dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_UINT32; i++) {
        dbus_uint32_t x;
        dbus_message_iter_get_basic(&a, &x);
        st[i] = x;
        dbus_message_iter_next(&a);
    }
    return true;
}

// Parses a (so) struct at the iterator.
static bool iter_ref(DBusMessageIter *it, aref *out) {
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_STRUCT) return false;
    DBusMessageIter s;
    dbus_message_iter_recurse(it, &s);
    const char *bus = NULL, *path = NULL;
    if (dbus_message_iter_get_arg_type(&s) != DBUS_TYPE_STRING) return false;
    dbus_message_iter_get_basic(&s, &bus);
    dbus_message_iter_next(&s);
    if (dbus_message_iter_get_arg_type(&s) != DBUS_TYPE_OBJECT_PATH) return false;
    dbus_message_iter_get_basic(&s, &path);
    out->bus = strdup(bus ? bus : "");
    out->path = strdup(path ? path : PATH_NULL);
    return true;
}

static bool rep_ref(DBusMessage *r, aref *out) {
    DBusMessageIter it, v;
    if (!iter_first(r, &it)) return false;
    iter_unvariant(&it, &v);
    return iter_ref(&v, out);
}

// a(so) -> array of refs (skips null refs). Caps at `max` entries.
static int rep_refs(DBusMessage *r, aref **out, int max) {
    *out = NULL;
    DBusMessageIter it, a;
    if (!iter_first(r, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return 0;
    dbus_message_iter_recurse(&it, &a);
    int n = 0, cap = 0;
    aref *v = NULL;
    while (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_STRUCT && n < max) {
        aref x = {0};
        if (iter_ref(&a, &x)) {
            if (aref_valid(&x)) {
                if (n == cap) {
                    cap = cap ? cap * 2 : 16;
                    aref *nv = realloc(v, (size_t)cap * sizeof *v);
                    if (!nv) { aref_free(&x); break; }
                    v = nv;
                }
                v[n++] = x;
            } else {
                aref_free(&x);
            }
        }
        dbus_message_iter_next(&a);
    }
    *out = v;
    return n;
}

static unsigned rep_ifaces(DBusMessage *r) {
    DBusMessageIter it, a;
    unsigned m = 0;
    if (!iter_first(r, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return 0;
    dbus_message_iter_recurse(&it, &a);
    while (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_STRING) {
        const char *s = NULL;
        dbus_message_iter_get_basic(&a, &s);
        if (s) {
            if (!strcmp(s, IF_TEXT)) m |= IFB_TEXT;
            else if (!strcmp(s, IF_EDITABLE)) m |= IFB_EDITABLE;
            else if (!strcmp(s, IF_DOCUMENT)) m |= IFB_DOCUMENT;
            else if (!strcmp(s, IF_COMPONENT)) m |= IFB_COMPONENT;
            else if (!strcmp(s, "org.a11y.atspi.Hypertext")) m |= IFB_HYPERTEXT;
        }
        dbus_message_iter_next(&a);
    }
    return m;
}

static void refs_free(aref *v, int n) {
    for (int i = 0; i < n; i++) aref_free(&v[i]);
    free(v);
}

// ---------------------------------------------------------------------------
// Application / window discovery
// ---------------------------------------------------------------------------

static int pid_cache_get(const char *bus) {
    int pid = 0;
    pthread_mutex_lock(&g_pid_mu);
    for (int i = 0; i < g_npids; i++)
        if (!strcmp(g_pids[i].name, bus)) { pid = g_pids[i].pid; break; }
    pthread_mutex_unlock(&g_pid_mu);
    return pid;
}

static void pid_cache_put(const char *bus, int pid) {
    if (strlen(bus) >= sizeof g_pids[0].name) return;
    pthread_mutex_lock(&g_pid_mu);
    int slot;
    if (g_npids < PID_CACHE) slot = g_npids++;
    else slot = g_pid_next++ % PID_CACHE;
    snprintf(g_pids[slot].name, sizeof g_pids[slot].name, "%s", bus);
    g_pids[slot].pid = pid;
    pthread_mutex_unlock(&g_pid_mu);
}

static DBusMessage *m_pid_query(const char *bus) {
    if (!bus || !dbus_validate_bus_name(bus, NULL)) return NULL;
    DBusMessage *m = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus", "GetConnectionUnixProcessID");
    if (m) dbus_message_append_args(m, DBUS_TYPE_STRING, &bus, DBUS_TYPE_INVALID);
    return m;
}

// Resolves pids for all refs (by bus name); pids[i] = -1 if unknown.
static void resolve_pids(DBusConnection *c, aref *v, int n, int *pids, int64_t deadline) {
    bcall *b = calloc((size_t)n + 1, sizeof *b);
    if (!b) return;
    for (int i = 0; i < n; i++) {
        pids[i] = pid_cache_get(v[i].bus);
        if (!pids[i]) b[i].m = m_pid_query(v[i].bus);
    }
    batch_exec(c, b, n, deadline);
    for (int i = 0; i < n; i++) {
        if (pids[i]) continue;
        int64_t p;
        if (b[i].r && rep_int(b[i].r, &p) && p > 0) {
            pids[i] = (int)p;
            pid_cache_put(v[i].bus, (int)p);
        } else {
            pids[i] = -1;
        }
    }
    batch_free(b, n);
}

static int bus_pid(DBusConnection *c, const char *bus, int64_t deadline) {
    aref r = {(char *)bus, (char *)PATH_ROOT};
    int pid = -1;
    resolve_pids(c, &r, 1, &pid, deadline);
    return pid;
}

static int list_apps(DBusConnection *c, aref **apps, int64_t deadline) {
    aref reg = {(char *)REGISTRY_BUS, (char *)PATH_ROOT};
    DBusMessage *m = m_call(&reg, IF_ACC, "GetChildren");
    DBusMessage *r = call_block(c, m, (int)(deadline - now_ms()));
    int n = rep_refs(r, apps, 512);
    if (r) dbus_message_unref(r);
    return n;
}

// Application roots whose owning process is `pid`.
static int apps_for_pid(DBusConnection *c, int pid, aref **out, int64_t deadline) {
    aref *apps = NULL;
    int n = list_apps(c, &apps, deadline);
    *out = NULL;
    if (n <= 0) return 0;
    int *pids = calloc((size_t)n, sizeof(int));
    if (!pids) {
        refs_free(apps, n);
        return 0;
    }
    resolve_pids(c, apps, n, pids, deadline);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (pids[i] == pid && pid != getpid()) apps[k++] = apps[i];
        else aref_free(&apps[i]);
    }
    free(pids);
    if (!k) {
        free(apps);
        return 0;
    }
    *out = apps;
    return k;
}

typedef struct {
    aref r;
    uint32_t st[2];
    char *name;
    int app_idx;
} wininfo;

// Collects the top-level windows of the given applications with state + name.
static int collect_windows(DBusConnection *c, aref *apps, int napps, wininfo **out, int64_t deadline) {
    *out = NULL;
    bcall *b = calloc((size_t)napps + 1, sizeof *b);
    if (!b) return 0;
    for (int i = 0; i < napps; i++) b[i].m = m_call(&apps[i], IF_ACC, "GetChildren");
    batch_exec(c, b, napps, deadline);
    wininfo *w = NULL;
    int n = 0, cap = 0;
    for (int i = 0; i < napps; i++) {
        aref *kids = NULL;
        int nk = rep_refs(b[i].r, &kids, 64);
        for (int j = 0; j < nk; j++) {
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                wininfo *nw = realloc(w, (size_t)cap * sizeof *w);
                if (!nw) { aref_free(&kids[j]); continue; }
                w = nw;
            }
            memset(&w[n], 0, sizeof w[n]);
            w[n].r = kids[j];
            w[n].app_idx = i;
            n++;
        }
        free(kids);
    }
    batch_free(b, napps);
    if (!n) return 0;
    b = calloc((size_t)n * 2 + 1, sizeof *b);
    if (!b) {
        *out = w;
        return n;
    }
    for (int i = 0; i < n; i++) {
        b[2 * i].m = m_call(&w[i].r, IF_ACC, "GetState");
        b[2 * i + 1].m = m_prop(&w[i].r, IF_ACC, "Name");
    }
    batch_exec(c, b, 2 * n, deadline);
    for (int i = 0; i < n; i++) {
        rep_state(b[2 * i].r, w[i].st);
        w[i].name = rep_string(b[2 * i + 1].r);
    }
    batch_free(b, 2 * n);
    *out = w;
    return n;
}

static void windows_free(wininfo *w, int n) {
    for (int i = 0; i < n; i++) {
        aref_free(&w[i].r);
        free(w[i].name);
    }
    free(w);
}

// Picks the most plausible foreground window of the foreground process.
static bool find_foreground_window(DBusConnection *c, int pid, const char *title, aref *out, int64_t deadline) {
    memset(out, 0, sizeof *out);
    aref *apps = NULL;
    int napps = apps_for_pid(c, pid, &apps, deadline);
    if (!napps) return false;
    wininfo *w = NULL;
    int n = collect_windows(c, apps, napps, &w, deadline);
    int best = -1, best_score = -1;
    aref last;
    pthread_mutex_lock(&g_ev_mu);
    last.bus = g_active_win.bus ? strdup(g_active_win.bus) : NULL;
    last.path = g_active_win.path ? strdup(g_active_win.path) : NULL;
    pthread_mutex_unlock(&g_ev_mu);
    for (int i = 0; i < n; i++) {
        if (st_has(w[i].st, ST_DEFUNCT)) continue;
        int s = 0;
        if (st_has(w[i].st, ST_ACTIVE)) s += 8;
        if (title && title[0] && w[i].name && !strcmp(w[i].name, title)) s += 4;
        if (str_eq(last.bus, w[i].r.bus) && str_eq(last.path, w[i].r.path)) s += 2;
        if (st_has(w[i].st, ST_SHOWING) || st_has(w[i].st, ST_VISIBLE)) s += 1;
        if (s > best_score) {
            best_score = s;
            best = i;
        }
    }
    aref_free(&last);
    bool ok = false;
    if (best >= 0) {
        out->bus = strdup(w[best].r.bus);
        out->path = strdup(w[best].r.path);
        ok = true;
    }
    windows_free(w, n);
    refs_free(apps, napps);
    return ok;
}

bool a11y_active_window(fg_window_t *out, int timeout_ms) {
    memset(out, 0, sizeof *out);
    DBusConnection *c = conn_get();
    if (!c) return false;
    int64_t deadline = now_ms() + timeout_ms;
    aref *apps = NULL;
    int napps = list_apps(c, &apps, deadline);
    bool ok = false;
    if (napps > 0) {
        wininfo *w = NULL;
        int n = collect_windows(c, apps, napps, &w, deadline);
        for (int i = 0; i < n && !ok; i++) {
            if (!st_has(w[i].st, ST_ACTIVE) || st_has(w[i].st, ST_DEFUNCT)) continue;
            int pid = bus_pid(c, w[i].r.bus, deadline);
            if (pid <= 0) continue;
            out->pid = pid;
            out->title = xstrdup(w[i].name);
            out->cls = xstrdup("");
            out->visible = st_has(w[i].st, ST_SHOWING) || st_has(w[i].st, ST_VISIBLE);
            DBusMessage *m = m_call(&w[i].r, IF_COMPONENT, "GetExtents");
            dbus_uint32_t coord = 0; // ATSPI_COORD_TYPE_SCREEN
            if (m) dbus_message_append_args(m, DBUS_TYPE_UINT32, &coord, DBUS_TYPE_INVALID);
            DBusMessage *r = call_block(c, m, (int)(deadline - now_ms()));
            DBusMessageIter it, s;
            if (iter_first(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRUCT) {
                dbus_message_iter_recurse(&it, &s);
                int vals[4] = {0};
                for (int k = 0; k < 4 && dbus_message_iter_get_arg_type(&s) == DBUS_TYPE_INT32; k++) {
                    dbus_int32_t x;
                    dbus_message_iter_get_basic(&s, &x);
                    vals[k] = x;
                    dbus_message_iter_next(&s);
                }
                out->x = vals[0];
                out->y = vals[1];
                out->w = vals[2];
                out->h = vals[3];
            }
            if (r) dbus_message_unref(r);
            ok = true;
        }
        windows_free(w, n);
        refs_free(apps, napps);
    }
    conn_put(c);
    return ok;
}

// ---------------------------------------------------------------------------
// Batched tree walking
// ---------------------------------------------------------------------------

typedef struct {
    aref r;
    int parent, depth;
    uint32_t st[2];
    bool st_ok;
    int role;
    unsigned ifaces;
    int *kids;
    int nkids;
    bool expanded;
    char *text;
    char *name;
} tnode;

typedef struct {
    tnode *v;
    int n, cap;
} tree;

static int tree_add(tree *t, aref r, int parent, int depth) {
    if (t->n == t->cap) {
        int nc = t->cap ? t->cap * 2 : 64;
        tnode *nv = realloc(t->v, (size_t)nc * sizeof *nv);
        if (!nv) {
            aref_free(&r);
            return -1;
        }
        t->v = nv;
        t->cap = nc;
    }
    tnode *x = &t->v[t->n];
    memset(x, 0, sizeof *x);
    x->r = r;
    x->parent = parent;
    x->depth = depth;
    x->role = -1;
    return t->n++;
}

static void tree_free(tree *t) {
    for (int i = 0; i < t->n; i++) {
        aref_free(&t->v[i].r);
        free(t->v[i].kids);
        free(t->v[i].text);
        free(t->v[i].name);
    }
    free(t->v);
    memset(t, 0, sizeof *t);
}

enum { TF_STATE = 1, TF_ROLE = 2, TF_IFACES = 4, TF_NAME = 8 };

static void tree_fetch(DBusConnection *c, tree *t, int from, int to, unsigned flags, int64_t deadline) {
    int per = 4, n = to - from;
    if (n <= 0) return;
    bcall *b = calloc((size_t)n * per + 1, sizeof *b);
    if (!b) return;
    for (int i = 0; i < n; i++) {
        aref *r = &t->v[from + i].r;
        if (flags & TF_STATE) b[i * per + 0].m = m_call(r, IF_ACC, "GetState");
        if (flags & TF_ROLE) b[i * per + 1].m = m_call(r, IF_ACC, "GetRole");
        if (flags & TF_IFACES) b[i * per + 2].m = m_call(r, IF_ACC, "GetInterfaces");
        if (flags & TF_NAME) b[i * per + 3].m = m_prop(r, IF_ACC, "Name");
    }
    batch_exec(c, b, n * per, deadline);
    for (int i = 0; i < n; i++) {
        tnode *x = &t->v[from + i];
        if (b[i * per + 0].r) x->st_ok = rep_state(b[i * per + 0].r, x->st);
        int64_t role;
        if (b[i * per + 1].r && rep_int(b[i * per + 1].r, &role)) x->role = (int)role;
        if (b[i * per + 2].r) x->ifaces = rep_ifaces(b[i * per + 2].r);
        if (b[i * per + 3].r) x->name = rep_string(b[i * per + 3].r);
    }
    batch_free(b, n * per);
}

static void tree_fetch_names(DBusConnection *c, tree *t, const int *idx, int n, int64_t deadline) {
    bcall *b = calloc((size_t)n + 1, sizeof *b);
    if (!b) return;
    for (int i = 0; i < n; i++) b[i].m = m_prop(&t->v[idx[i]].r, IF_ACC, "Name");
    batch_exec(c, b, n, deadline);
    for (int i = 0; i < n; i++) {
        free(t->v[idx[i]].name);
        t->v[idx[i]].name = rep_string(b[i].r);
    }
    batch_free(b, n);
}

// Expands the given nodes (GetChildren), appending children to the tree.
static void tree_expand(DBusConnection *c, tree *t, const int *idx, int n, int max_breadth, int max_nodes,
                        int64_t deadline) {
    if (n <= 0) return;
    bcall *b = calloc((size_t)n + 1, sizeof *b);
    if (!b) return;
    for (int i = 0; i < n; i++) b[i].m = m_call(&t->v[idx[i]].r, IF_ACC, "GetChildren");
    batch_exec(c, b, n, deadline);
    for (int i = 0; i < n; i++) {
        int p = idx[i];
        t->v[p].expanded = true;
        aref *kids = NULL;
        int nk = rep_refs(b[i].r, &kids, max_breadth);
        int *ki = nk ? calloc((size_t)nk, sizeof(int)) : NULL;
        int got = 0;
        for (int j = 0; j < nk; j++) {
            if (t->n >= max_nodes || !ki) {
                aref_free(&kids[j]);
                continue;
            }
            int id = tree_add(t, kids[j], p, t->v[p].depth + 1);
            if (id >= 0) ki[got++] = id;
        }
        free(kids);
        t->v[p].kids = ki;
        t->v[p].nkids = got;
    }
    batch_free(b, n);
}

// ---------------------------------------------------------------------------
// Focused element discovery
// ---------------------------------------------------------------------------

// Checks the cached focus-event object; returns true if it's still focused and
// belongs to `pid`.
static bool cached_focus_for_pid(DBusConnection *c, int pid, aref *out, int64_t deadline) {
    aref f = {0};
    pthread_mutex_lock(&g_ev_mu);
    if (g_focus.bus && g_focus.path) {
        f.bus = strdup(g_focus.bus);
        f.path = strdup(g_focus.path);
    }
    pthread_mutex_unlock(&g_ev_mu);
    if (!f.bus || !aref_valid(&f)) {
        aref_free(&f);
        return false;
    }
    int fp = bus_pid(c, f.bus, deadline);
    if (fp != pid || fp == getpid()) {
        aref_free(&f);
        return false;
    }
    DBusMessage *r = call_block(c, m_call(&f, IF_ACC, "GetState"), (int)(deadline - now_ms()));
    uint32_t st[2];
    bool ok = r && rep_state(r, st) && st_has(st, ST_FOCUSED) && !st_has(st, ST_DEFUNCT);
    if (r) dbus_message_unref(r);
    if (!ok) {
        aref_free(&f);
        return false;
    }
    *out = f;
    return true;
}

// Bounded BFS below `root` for an object with STATE_FOCUSED.
static bool walk_for_focus(DBusConnection *c, const aref *root, aref *out, int64_t deadline) {
    const int max_depth = 24, max_breadth = 64, max_nodes = 600;
    tree t = {0};
    aref r0 = {strdup(root->bus), strdup(root->path)};
    tree_add(&t, r0, -1, 0);
    int level_start = 0, found = -1;
    while (level_start < t.n && found < 0 && now_ms() < deadline) {
        int level_end = t.n;
        tree_fetch(c, &t, level_start, level_end, TF_STATE, deadline);
        int *exp = calloc((size_t)(level_end - level_start) + 1, sizeof(int));
        int ne = 0;
        for (int i = level_start; i < level_end && exp; i++) {
            tnode *x = &t.v[i];
            if (!x->st_ok || st_has(x->st, ST_DEFUNCT)) continue;
            if (i > 0 && st_has(x->st, ST_FOCUSED)) {
                found = i;
                break;
            }
            // Only descend into what is on screen; skip huge virtualised containers.
            if (i > 0 && !st_has(x->st, ST_SHOWING) && !st_has(x->st, ST_VISIBLE)) continue;
            if (st_has(x->st, ST_MANAGES_DESCENDANTS)) continue;
            if (x->depth < max_depth) exp[ne++] = i;
        }
        if (found < 0 && ne) tree_expand(c, &t, exp, ne, max_breadth, max_nodes, deadline);
        free(exp);
        level_start = level_end;
    }
    bool ok = false;
    if (found >= 0) {
        out->bus = strdup(t.v[found].r.bus);
        out->path = strdup(t.v[found].r.path);
        ok = true;
    }
    tree_free(&t);
    return ok;
}

// Focused element of the foreground app (or, like the x64 helper, the active
// window when nothing inside it reports focus). *is_window tells which.
static bool find_focus(DBusConnection *c, const fg_window_t *fg, aref *out, bool *is_window, int64_t deadline) {
    memset(out, 0, sizeof *out);
    *is_window = false;
    if (fg->pid <= 0 || fg->pid == getpid()) return false;
    pump_events(c);
    if (cached_focus_for_pid(c, fg->pid, out, deadline)) return true;
    aref win;
    if (!find_foreground_window(c, fg->pid, fg->title, &win, deadline)) return false;
    if (walk_for_focus(c, &win, out, deadline)) {
        aref_free(&win);
        return true;
    }
    *out = win;
    *is_window = true;
    return true;
}

static void get_fg(fg_window_t *fg) {
    if (hypr_active_window(fg, 250) < 0) {
        fg_window_free(fg);
        a11y_active_window(fg, 250);
    }
}

// ---------------------------------------------------------------------------
// Snapshot (getFocusedElementInfo)
// ---------------------------------------------------------------------------

static const char *ax_role(int role) {
    switch (role) {
    case 7: return "AXCheckBox";
    case 11: return "AXComboBox";
    case 20: case 39: case 85: case 99: return "AXGroup";
    case 27: return "AXImage";
    case 31: return "AXList";
    case 32: return "AXListItem";
    case 40: case 79: return "AXTextField";
    case 43: return "AXButton";
    case 44: return "AXRadioButton";
    case 60: case 61: case 77: case 82: case 94: case 95: case 96: return "AXTextArea";
    case 88: return "AXLink";
    }
    return NULL;
}

static bool is_editable_from_snapshot(const uint32_t st[2], int role) {
    if (st_has(st, ST_READ_ONLY)) return false;
    if (st_has(st, ST_EDITABLE)) return true;
    return role == R_PASSWORD_TEXT || role == R_TERMINAL || role == R_TEXT || role == R_EDITBAR || role == R_ENTRY;
}

static bool is_explicitly_non_text(int role, const uint32_t st[2], bool st_ok) {
    switch (role) {
    case 1: case 2: case 3: case 4: case 7: case 8: case 13: case 15: case 20: case 26: case 27: case 29:
    case 33: case 34: case 35: case 37: case 38: case 41: case 42: case 43: case 44: case 45: case 48:
    case 50: case 51: case 54: case 59: case 62: case 64: case 74: case 80: case 81: case 86: case 88:
    case 89: case 100: case 101: case 102: case 103: case 104: case 106: case 107: case 112: case 114:
    case 115: case 116: case 129: case 130:
        return true;
    case R_LIST_ITEM: case R_TREE_ITEM:
        if (!st_ok) return true;
        if (st_has(st, ST_ENABLED)) return false;
        return !st_has(st, ST_FOCUSED);
    }
    return false;
}

static const char k_default_element[] =
    "{\"role\":\"AXUnknown\",\"subrole\":\"\",\"label\":\"\",\"title\":\"\",\"help\":\"\",\"editable\":false,"
    "\"focused\":false,\"enabled\":false,\"multiline\":false}";

static char *snapshot_json(DBusConnection *c, const aref *el, int64_t deadline, int *role_out) {
    int role = -1;
    uint32_t st[2] = {0, 0};
    bool st_ok = false;
    char *name = NULL, *desc = NULL;
    if (el && aref_valid(el)) {
        bcall b[4] = {{0}};
        b[0].m = m_call(el, IF_ACC, "GetRole");
        b[1].m = m_call(el, IF_ACC, "GetState");
        b[2].m = m_prop(el, IF_ACC, "Name");
        b[3].m = m_prop(el, IF_ACC, "Description");
        batch_exec(c, b, 4, deadline);
        int64_t v;
        if (b[0].r && rep_int(b[0].r, &v)) role = (int)v;
        if (b[1].r) st_ok = rep_state(b[1].r, st);
        name = rep_string(b[2].r);
        desc = rep_string(b[3].r);
        for (int i = 0; i < 4; i++)
            if (b[i].r) dbus_message_unref(b[i].r);
    }
    if (role_out) *role_out = role;
    const char *role_name = (role >= 0 && role <= 0x82 && role != 70) ? k_role_names[role] : "";
    sb_t s;
    sb_init(&s);
    const char *ax = ax_role(role);
    char axbuf[96];
    if (!ax) {
        if (role_name[0]) {
            snprintf(axbuf, sizeof axbuf, "AX%s", role_name);
            ax = axbuf;
        } else {
            ax = "AXUnknown";
        }
    }
    bool editable = false, focused = false, enabled = false, multiline = false, non_text = false;
    if (el && role >= 0) {
        if (st_ok) {
            editable = is_editable_from_snapshot(st, role);
            focused = st_has(st, ST_FOCUSED);
            enabled = st_has(st, ST_ENABLED);
            if (role > 61) multiline = (role == R_DOCUMENT_TEXT || role == R_DOCUMENT_WEB) || st_has(st, ST_MULTI_LINE);
            else if (role >= 60) multiline = true;
            else multiline = st_has(st, ST_MULTI_LINE);
        } else {
            multiline = role == R_TERMINAL || role == R_TEXT;
        }
        non_text = is_explicitly_non_text(role, st, st_ok);
    }
    sb_putc(&s, '{');
    sb_json_kv_str(&s, "role", ax);
    sb_putc(&s, ',');
    sb_json_kv_str(&s, "subrole", role_name);
    sb_putc(&s, ',');
    sb_json_kv_str(&s, "label", name ? name : "");
    sb_putc(&s, ',');
    sb_json_kv_str(&s, "title", name ? name : "");
    sb_putc(&s, ',');
    sb_json_kv_str(&s, "help", desc ? desc : "");
    sb_printf(&s, ",\"editable\":%s,\"focused\":%s,\"enabled\":%s,\"multiline\":%s,", editable ? "true" : "false",
              focused ? "true" : "false", enabled ? "true" : "false", multiline ? "true" : "false");
    sb_json_kv_str(&s, "busName", el && el->bus ? el->bus : "");
    sb_putc(&s, ',');
    sb_json_kv_str(&s, "objectPath", el && el->path ? el->path : "");
    sb_printf(&s, ",\"explicitNonText\":%s}", non_text ? "true" : "false");
    free(name);
    free(desc);
    return sb_take(&s);
}

char *a11y_focus_snapshot_json(int budget_ms) {
    int64_t deadline = now_ms() + budget_ms;
    fg_window_t fg;
    get_fg(&fg);
    DBusConnection *c = conn_get();
    if (!c) {
        fg_window_free(&fg);
        return xstrdup(k_default_element);
    }
    aref el = {0};
    bool is_win = false;
    bool found = find_focus(c, &fg, &el, &is_win, deadline);
    // Give the description calls a little extra room past the search budget.
    char *json = snapshot_json(c, found ? &el : NULL, (found ? deadline : now_ms()) + 100, NULL);
    dbg("snapshot pid=%d found=%d win=%d -> %s", fg.pid, found, is_win, json ? json : "(null)");
    aref_free(&el);
    fg_window_free(&fg);
    conn_put(c);
    return json ? json : xstrdup(k_default_element);
}

// ---------------------------------------------------------------------------
// Surrounding text (getFocusedElementRelatedContent)
// ---------------------------------------------------------------------------

static char *err_json(const char *msg) {
    sb_t s;
    sb_init(&s);
    sb_puts(&s, "{\"error\":\"");
    sb_json_escape(&s, msg);
    sb_puts(&s, "\"}");
    char *r = sb_take(&s);
    return r ? r : xstrdup("{\"error\":\"Out of memory\"}");
}

static unsigned get_ifaces(DBusConnection *c, const aref *r, int64_t deadline) {
    DBusMessage *rep = call_block(c, m_call(r, IF_ACC, "GetInterfaces"), (int)(deadline - now_ms()));
    unsigned m = rep ? rep_ifaces(rep) : 0;
    if (rep) dbus_message_unref(rep);
    return m;
}

static bool text_counts(DBusConnection *c, const aref *r, int64_t *count, int64_t *caret, int64_t deadline) {
    bcall b[2] = {{0}};
    b[0].m = m_prop(r, IF_TEXT, "CharacterCount");
    b[1].m = m_prop(r, IF_TEXT, "CaretOffset");
    batch_exec(c, b, 2, deadline);
    bool ok = b[0].r && rep_int(b[0].r, count);
    if (!(b[1].r && rep_int(b[1].r, caret))) *caret = -1;
    for (int i = 0; i < 2; i++)
        if (b[i].r) dbus_message_unref(b[i].r);
    return ok;
}

static DBusMessage *m_get_text(const aref *r, int start, int end) {
    DBusMessage *m = m_call(r, IF_TEXT, "GetText");
    dbus_int32_t s = start, e = end;
    if (m) dbus_message_append_args(m, DBUS_TYPE_INT32, &s, DBUS_TYPE_INT32, &e, DBUS_TYPE_INVALID);
    return m;
}

static void strip_fffc(char *s) {
    if (!s) return;
    size_t n = strlen(s), w = 0;
    for (size_t k = 0; k < n;) {
        if (k + 2 < n && (unsigned char)s[k] == 0xEF && (unsigned char)s[k + 1] == 0xBF && (unsigned char)s[k + 2] == 0xBC) {
            k += 3;
            continue;
        }
        s[w++] = s[k++];
    }
    s[w] = 0;
}

char *a11y_related_content_json(int before, int after, int timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = 1000;
    if (before < 0) before = 0;
    if (after < 0) after = 0;
    int64_t t0 = now_ms();
    // Leave headroom so the reply reaches JS before its own timer fires.
    int64_t deadline = t0 + (timeout_ms > 200 ? timeout_ms - 60 : timeout_ms * 3 / 4);
    fg_window_t fg;
    get_fg(&fg);
    DBusConnection *c = conn_get();
    if (!c) {
        fg_window_free(&fg);
        return err_json("Accessibility not available");
    }
    char *result = NULL;
    aref el = {0};
    bool is_win = false;
    if (!find_focus(c, &fg, &el, &is_win, deadline) || is_win) {
        result = err_json("Failed to get focused element");
        goto out;
    }
    // Locate a Text implementation on the element or a close ancestor.
    aref target = {strdup(el.bus), strdup(el.path)};
    unsigned ifs = get_ifaces(c, &target, deadline);
    for (int lvl = 0; !(ifs & IFB_TEXT) && lvl < 3 && now_ms() < deadline; lvl++) {
        DBusMessage *r = call_block(c, m_prop(&target, IF_ACC, "Parent"), (int)(deadline - now_ms()));
        aref p = {0};
        bool ok = r && rep_ref(r, &p) && aref_valid(&p) && str_eq(p.bus, target.bus);
        if (r) dbus_message_unref(r);
        aref_free(&target);
        if (!ok) {
            aref_free(&p);
            break;
        }
        target = p;
        ifs = get_ifaces(c, &target, deadline);
    }
    if (!target.bus || !(ifs & IFB_TEXT)) {
        aref_free(&target);
        result = err_json("No text interface");
        goto out;
    }
    {
        DBusMessage *rr = call_block(c, m_call(&target, IF_ACC, "GetRole"), (int)(deadline - now_ms()));
        int64_t role = -1;
        if (rr) {
            rep_int(rr, &role);
            dbus_message_unref(rr);
        }
        if (role == R_PASSWORD_TEXT) {
            aref_free(&target);
            result = err_json("No readable text on focused element");
            goto out;
        }
    }
    int64_t count = 0, caret = -1;
    if (!text_counts(c, &target, &count, &caret, deadline)) {
        aref_free(&target);
        result = err_json("No readable text on focused element");
        goto out;
    }
    if (count < 0) count = 0;
    if (caret < 0 || caret > count) caret = count;
    int start = (int)(caret - before > 0 ? caret - before : 0);
    int end = (int)(caret + after < count ? caret + after : count);
    bcall b[3] = {{0}};
    b[0].m = m_get_text(&target, start, (int)caret);
    b[1].m = m_get_text(&target, (int)caret, end);
    if (caret < count) b[2].m = m_get_text(&target, (int)caret, (int)caret + 1);
    batch_exec(c, b, 3, deadline);
    bool timed_out = now_ms() >= deadline;
    char *bt = rep_string(b[0].r), *at = rep_string(b[1].r), *ft = rep_string(b[2].r);
    // Embedded-object placeholders (U+FFFC) carry no text; drop them.
    strip_fffc(bt);
    strip_fffc(at);
    strip_fffc(ft);
    for (int i = 0; i < 3; i++)
        if (b[i].r) dbus_message_unref(b[i].r);
    int64_t count2 = count, caret2;
    if (!timed_out && text_counts(c, &target, &count2, &caret2, deadline + 50) && count2 != count) {
        free(bt); free(at); free(ft);
        aref_free(&target);
        result = err_json("Text collection changed");
        goto out;
    }
    aref_free(&target);
    if (!bt && !at && caret > 0) {
        free(ft);
        result = err_json("No readable text on focused element");
        goto out;
    }
    {
        sb_t s;
        sb_init(&s);
        sb_putc(&s, '{');
        sb_json_kv_str(&s, "beforeContent", bt ? bt : "");
        sb_putc(&s, ',');
        sb_json_kv_str(&s, "afterContent", at ? at : "");
        sb_putc(&s, ',');
        sb_json_kv_str(&s, "focusedElementText", ft ? ft : "");
        sb_printf(&s, ",\"beforeCharsRequested\":%d,\"afterCharsRequested\":%d", before, after);
        sb_printf(&s, ",\"beforeCharsActual\":%d,\"afterCharsActual\":%d",
                  bt ? (int)utf8_count(bt, strlen(bt)) : 0, at ? (int)utf8_count(at, strlen(at)) : 0);
        sb_printf(&s, ",\"fullTextLength\":%d,\"focusedElementPosition\":%d", (int)count, (int)caret);
        sb_printf(&s, ",\"strategy\":\"AtspiText\",\"timeout\":%s}", timed_out ? "true" : "false");
        result = sb_take(&s);
    }
    free(bt);
    free(at);
    free(ft);
out:
    dbg("related (%d,%d,%d) in %lldms -> %.200s", before, after, timeout_ms, (long long)(now_ms() - t0),
        result ? result : "(null)");
    aref_free(&el);
    fg_window_free(&fg);
    conn_put(c);
    return result ? result : err_json("Failed to get related content");
}

// ---------------------------------------------------------------------------
// Visible text (getFocusedVisibleText)
// ---------------------------------------------------------------------------

static bool role_is_block(int role) {
    switch (role) {
    case 73: /* paragraph */ case 83: /* heading */ case 85: /* section */ case 31: case 32: /* list, item */
    case 55: case 90: case 56: /* table, row, cell */ case 87: /* form */ case 105: case 109: case 110:
    case 71: case 72: /* header, footer */ case 95: case 82: case 94: case 39: case 16: case 69: case 23:
    case 99: case 98: case 121: case 122: case 123: case 29: /* label */
        return true;
    }
    return false;
}

static bool role_skip_text(int role) {
    // Never report contents of password fields; skip chrome that is not content.
    return role == R_PASSWORD_TEXT || role == 48 /* scroll bar */ || role == 64 /* tool tip */;
}

typedef struct {
    sb_t out;
    size_t cp;      // code points emitted
    size_t max_cp;
    bool strict;    // require STATE_SHOWING (toolkit reports it on the window)
} vis_ctx;

static void vis_emit(vis_ctx *v, const char *s, size_t n) {
    if (v->cp >= v->max_cp || !n) return;
    size_t room = v->max_cp - v->cp;
    size_t bytes = utf8_prefix_bytes(s, n, room);
    sb_putn(&v->out, s, bytes);
    v->cp += utf8_count(s, bytes);
}

static char vis_last(const vis_ctx *v) { return v->out.n ? v->out.p[v->out.n - 1] : '\n'; }

static void vis_newline(vis_ctx *v) {
    if (vis_last(v) != '\n') vis_emit(v, "\n", 1);
}

static void vis_space(vis_ctx *v) {
    char c = vis_last(v);
    if (c != '\n' && c != ' ' && c != '\t') vis_emit(v, " ", 1);
}

static bool node_visible(const tnode *x) {
    return x->st_ok && !st_has(x->st, ST_DEFUNCT) && (st_has(x->st, ST_SHOWING) || st_has(x->st, ST_VISIBLE));
}

static bool node_visible_mode(const tnode *x, bool strict) {
    if (!node_visible(x)) return false;
    return !strict || st_has(x->st, ST_SHOWING);
}

static bool is_fffc(const char *s, size_t k, size_t n) {
    return k + 2 < n && (unsigned char)s[k] == 0xEF && (unsigned char)s[k + 1] == 0xBF && (unsigned char)s[k + 2] == 0xBC;
}

// Emits text in document order: a node's own text with each U+FFFC (embedded
// object) replaced by the corresponding child's text (hypertext model used by
// Chromium/Firefox); children not consumed that way follow afterwards,
// separated by a space (or newline for block-level roles).
static void vis_compose(vis_ctx *v, tree *t, int i, int depth) {
    if (depth > 200 || v->cp >= v->max_cp) return;
    tnode *x = &t->v[i];
    if (i > 0 && !node_visible_mode(x, v->strict)) return;
    int next_child = 0;
    bool block = role_is_block(x->role);
    if (block) vis_newline(v);
    if (x->text && x->text[0]) {
        const char *s = x->text;
        size_t n = strlen(s), run = 0;
        for (size_t k = 0; k < n;) {
            if (is_fffc(s, k, n)) {
                vis_emit(v, s + run, k - run);
                if (next_child < x->nkids) vis_compose(v, t, x->kids[next_child++], depth + 1);
                k += 3;
                run = k;
            } else {
                k++;
            }
        }
        vis_emit(v, s + run, n - run);
        if (next_child == 0 && x->nkids > 0 && !(x->ifaces & IFB_HYPERTEXT)) {
            // Plain text container (e.g. GTK label) - its children are not content.
            next_child = x->nkids;
        }
    }
    for (; next_child < x->nkids; next_child++) {
        vis_space(v);
        vis_compose(v, t, x->kids[next_child], depth + 1);
    }
    if (block) vis_newline(v);
}

char *a11y_visible_text(int max_len, int timeout_ms) {
    if (max_len <= 0) max_len = 20000;
    if (timeout_ms <= 0) timeout_ms = 2000;
    int64_t t0 = now_ms();
    int budget = timeout_ms > 400 ? timeout_ms - 150 : timeout_ms * 2 / 3;
    if (budget > 5000) budget = 5000;
    int64_t deadline = t0 + budget;
    fg_window_t fg;
    get_fg(&fg);
    if (fg.pid <= 0 || fg.pid == getpid()) {
        fg_window_free(&fg);
        return xstrdup("");
    }
    DBusConnection *c = conn_get();
    if (!c) {
        fg_window_free(&fg);
        return xstrdup("");
    }
    aref win;
    char *result = NULL;
    if (!find_foreground_window(c, fg.pid, fg.title, &win, deadline)) goto out;
    {
        const int max_depth = 64, max_breadth = 400, max_nodes = 6000;
        tree t = {0};
        tree_add(&t, win, -1, 0);
        // Leave ~1/3 of the budget for fetching texts.
        int64_t walk_deadline = t0 + (deadline - t0) * 2 / 3;
        int level_start = 0;
        bool strict = false;
        while (level_start < t.n && now_ms() < walk_deadline) {
            int level_end = t.n;
            tree_fetch(c, &t, level_start, level_end, TF_STATE | TF_ROLE | TF_IFACES, walk_deadline);
            if (level_start == 0) {
                strict = t.v[0].st_ok && st_has(t.v[0].st, ST_SHOWING);
            } else if (strict && t.v[level_start].depth == 1) {
                // Window reports SHOWING but none of its children do (e.g. it is
                // on another workspace): fall back to STATE_VISIBLE.
                bool any = false;
                for (int i = level_start; i < level_end && !any; i++)
                    any = t.v[i].st_ok && st_has(t.v[i].st, ST_SHOWING);
                strict = any;
            }
            int *exp = calloc((size_t)(level_end - level_start) + 1, sizeof(int));
            int ne = 0;
            for (int i = level_start; i < level_end && exp; i++) {
                tnode *x = &t.v[i];
                if (i > 0 && !node_visible_mode(x, strict)) continue;
                if (st_has(x->st, ST_MANAGES_DESCENDANTS) && x->role != R_DOCUMENT_WEB) continue;
                if (x->depth < max_depth) exp[ne++] = i;
            }
            if (ne) tree_expand(c, &t, exp, ne, max_breadth, max_nodes, walk_deadline);
            free(exp);
            level_start = level_end;
        }
        if (getenv("TYPELESS_CONTEXT_HELPER_DUMP_TREE"))
            for (int i = 0; i < t.n; i++)
                dbg("node %d p=%d d=%d role=%d show=%d vis=%d st_ok=%d kids=%d exp=%d if=%x %s", i, t.v[i].parent,
                    t.v[i].depth, t.v[i].role, st_has(t.v[i].st, ST_SHOWING), st_has(t.v[i].st, ST_VISIBLE),
                    t.v[i].st_ok, t.v[i].nkids, t.v[i].expanded, t.v[i].ifaces, t.v[i].r.path);
        // Fetch texts of visible text-bearing nodes.
        int *want = calloc((size_t)t.n + 1, sizeof(int));
        int nw = 0;
        for (int i = 0; i < t.n && want; i++) {
            tnode *x = &t.v[i];
            if ((x->ifaces & IFB_TEXT) && (i == 0 || node_visible_mode(x, strict)) && !role_skip_text(x->role))
                want[nw++] = i;
        }
        bcall *b = calloc((size_t)nw + 1, sizeof *b);
        if (b) {
            for (int k = 0; k < nw; k++) b[k].m = m_get_text(&t.v[want[k]].r, 0, -1);
            batch_exec(c, b, nw, deadline);
            for (int k = 0; k < nw; k++) t.v[want[k]].text = rep_string(b[k].r);
            batch_free(b, nw);
        }
        free(want);
        vis_ctx v;
        sb_init(&v.out);
        v.cp = 0;
        v.max_cp = (size_t)max_len;
        v.strict = strict;
        vis_compose(&v, &t, 0, 0);
        // Trim leading/trailing newlines.
        char *s = sb_take(&v.out);
        if (s) {
            size_t a = 0, n = strlen(s);
            while (a < n && (s[a] == '\n' || s[a] == ' ')) a++;
            while (n > a && (s[n - 1] == '\n' || s[n - 1] == ' ')) n--;
            result = xstrndup(s + a, n - a);
            free(s);
        }
        dbg("visible text: %d nodes, %zu cp, %lldms", t.n, v.cp, (long long)(now_ms() - t0));
        tree_free(&t);
    }
out:
    fg_window_free(&fg);
    conn_put(c);
    return result ? result : xstrdup("");
}

// ---------------------------------------------------------------------------
// Browser URL (BrowserHelper::GetWebPageInfo / FindDocumentUrlInActiveWindow)
// ---------------------------------------------------------------------------

#define URL_CACHE 16
static pthread_mutex_t g_url_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int pid;
    char *title;
    char *url;
    int64_t t;
} g_urls[URL_CACHE];

static void url_cache_clear(void) {
    pthread_mutex_lock(&g_url_mu);
    for (int i = 0; i < URL_CACHE; i++) {
        free(g_urls[i].title);
        free(g_urls[i].url);
        memset(&g_urls[i], 0, sizeof g_urls[i]);
    }
    pthread_mutex_unlock(&g_url_mu);
}

static bool url_cache_get(int pid, const char *title, char **url) {
    bool hit = false;
    int64_t t = now_ms();
    pthread_mutex_lock(&g_url_mu);
    for (int i = 0; i < URL_CACHE; i++) {
        if (g_urls[i].pid == pid && str_eq(g_urls[i].title, title)) {
            int64_t ttl = (g_urls[i].url && g_urls[i].url[0]) ? 30000 : 4000;
            if (t - g_urls[i].t < ttl) {
                *url = xstrdup(g_urls[i].url);
                hit = true;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_url_mu);
    return hit;
}

static void url_cache_put(int pid, const char *title, const char *url) {
    pthread_mutex_lock(&g_url_mu);
    int slot = 0;
    for (int i = 0; i < URL_CACHE; i++) {
        if (g_urls[i].pid == pid && str_eq(g_urls[i].title, title)) { slot = i; break; }
        if (g_urls[i].t < g_urls[slot].t) slot = i;
    }
    free(g_urls[slot].title);
    free(g_urls[slot].url);
    g_urls[slot].pid = pid;
    g_urls[slot].title = xstrdup(title);
    g_urls[slot].url = xstrdup(url);
    g_urls[slot].t = now_ms();
    pthread_mutex_unlock(&g_url_mu);
}

static bool looks_like_url(const char *s) {
    if (!s || !s[0]) return false;
    if (str_has_prefix(s, "about:")) return false;
    if (str_has_prefix(s, "http://") || str_has_prefix(s, "https://") || str_has_prefix(s, "file://")) return true;
    return false;
}

// Address-bar text without scheme ("example.com/path") -> https://...
static char *normalize_address_text(const char *s) {
    if (!s) return NULL;
    while (*s == ' ') s++;
    if (looks_like_url(s)) return strdup(s);
    if (strchr(s, ' ') || !strchr(s, '.')) return NULL;
    static const char *const tlds[] = {".com", ".org", ".net", ".edu", ".gov", ".io", ".co", ".dev", ".app",
                                       ".cn", ".uk", ".de", ".jp", ".fr", ".au", ".in", ".ru", ".br"};
    size_t host_len = strcspn(s, "/?#");
    char *host = xstrndup(s, host_len);
    bool ok = false;
    for (size_t i = 0; i < sizeof tlds / sizeof *tlds && !ok; i++)
        if (str_has_suffix(host, tlds[i]) || strstr(host, tlds[i])) ok = true;
    free(host);
    if (!ok) return NULL;
    size_t n = strlen(s) + 9;
    char *u = malloc(n);
    if (u) snprintf(u, n, "https://%s", s);
    return u;
}

static char *doc_url(DBusConnection *c, const aref *doc, int64_t deadline) {
    static const char *const keys[] = {"DocURL", "URI", "URL"};
    bcall b[3] = {{0}};
    for (int i = 0; i < 3; i++) {
        b[i].m = m_call(doc, IF_DOCUMENT, "GetAttributeValue");
        if (b[i].m) dbus_message_append_args(b[i].m, DBUS_TYPE_STRING, &keys[i], DBUS_TYPE_INVALID);
    }
    batch_exec(c, b, 3, deadline);
    char *url = NULL;
    for (int i = 0; i < 3; i++) {
        char *s = rep_string(b[i].r);
        if (!url && looks_like_url(s)) url = s;
        else free(s);
        if (b[i].r) dbus_message_unref(b[i].r);
    }
    return url;
}

char *a11y_browser_url(int pid, const char *window_title, int budget_ms) {
    if (pid <= 0 || pid == getpid()) return xstrdup("");
    char *cached = NULL;
    if (url_cache_get(pid, window_title, &cached)) return cached;
    int64_t t0 = now_ms(), deadline = t0 + budget_ms;
    DBusConnection *c = conn_get();
    if (!c) return xstrdup("");
    char *url = NULL;
    aref win;
    if (find_foreground_window(c, pid, window_title, &win, deadline)) {
        // Walk the browser chrome only: stop descending at web documents.
        const int max_depth = 30, max_breadth = 80, max_nodes = 700;
        tree t = {0};
        tree_add(&t, win, -1, 0);
        int level_start = 0;
        int docs[8], ndocs = 0;
        int entries[8], nent = 0;
        while (level_start < t.n && now_ms() < deadline) {
            int level_end = t.n;
            tree_fetch(c, &t, level_start, level_end, TF_STATE | TF_ROLE, deadline);
            int *exp = calloc((size_t)(level_end - level_start) + 1, sizeof(int));
            int ne = 0;
            for (int i = level_start; i < level_end && exp; i++) {
                tnode *x = &t.v[i];
                if (i > 0 && !node_visible(x)) continue;
                if (x->role == R_DOCUMENT_WEB || x->role == R_DOCUMENT_FRAME) {
                    if (ndocs < 8) docs[ndocs++] = i;
                    continue;
                }
                if (x->role == R_ENTRY && nent < 8) entries[nent++] = i;
                if (st_has(x->st, ST_MANAGES_DESCENDANTS)) continue;
                if (x->depth < max_depth) exp[ne++] = i;
            }
            if (ne && !ndocs) tree_expand(c, &t, exp, ne, max_breadth, max_nodes, deadline);
            free(exp);
            level_start = level_end;
            if (ndocs) break; // top-level document(s) found
        }
        if (ndocs > 1) {
            // Several documents (tabs / split view): prefer the one on screen whose
            // name (page title) prefixes the window title.
            tree_fetch_names(c, &t, docs, ndocs, deadline);
            int score[8];
            for (int k = 0; k < ndocs; k++) {
                tnode *x = &t.v[docs[k]];
                score[k] = (st_has(x->st, ST_SHOWING) ? 4 : 0) + (st_has(x->st, ST_FOCUSED) ? 2 : 0);
                if (x->name && x->name[0] && window_title && str_has_prefix(window_title, x->name)) score[k] += 8;
                dbg("doc %s%s score=%d name=%s", x->r.bus, x->r.path, score[k], x->name ? x->name : "");
            }
            for (int a = 1; a < ndocs; a++)
                for (int k = a; k > 0 && score[k] > score[k - 1]; k--) {
                    int ts = score[k]; score[k] = score[k - 1]; score[k - 1] = ts;
                    int td = docs[k]; docs[k] = docs[k - 1]; docs[k - 1] = td;
                }
        }
        for (int k = 0; k < ndocs && !url; k++) url = doc_url(c, &t.v[docs[k]].r, deadline + 50);
        if (!url && nent) {
            // Fallback: the address bar (entry named address/location/url/search).
            int ids[8];
            int ni = 0;
            for (int k = 0; k < nent; k++) ids[ni++] = entries[k];
            bcall b[16] = {{0}};
            for (int k = 0; k < ni; k++) {
                b[2 * k].m = m_prop(&t.v[ids[k]].r, IF_ACC, "Name");
                b[2 * k + 1].m = m_get_text(&t.v[ids[k]].r, 0, -1);
            }
            batch_exec(c, b, 2 * ni, deadline + 50);
            for (int k = 0; k < ni; k++) {
                char *name = rep_string(b[2 * k].r), *text = rep_string(b[2 * k + 1].r);
                if (name) str_lower_ascii(name);
                bool addr = name && (strstr(name, "address") || strstr(name, "location") || strstr(name, "url") ||
                                     strstr(name, "search with"));
                if (!url && addr) url = normalize_address_text(text);
                free(name);
                free(text);
            }
            for (int k = 0; k < 2 * ni; k++)
                if (b[k].r) dbus_message_unref(b[k].r);
        }
        tree_free(&t);
    }
    conn_put(c);
    dbg("browser url pid=%d in %lldms -> %s", pid, (long long)(now_ms() - t0), url ? url : "");
    // Cache negative results only if we actually got to look (not timed out).
    if (url || now_ms() < deadline) url_cache_put(pid, window_title, url ? url : "");
    return url ? url : xstrdup("");
}
