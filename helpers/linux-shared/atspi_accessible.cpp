// atspi_accessible.cpp - AT-SPI access layer for typeless-atspi-helper.
// Interoperable reimplementation; see atspi_accessible.h.
#include "atspi_accessible.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include <dbus/dbus.h>
#include <dlfcn.h>
#include <gio/gio.h>
#include <unistd.h>

namespace LinuxShared {

// ---------------------------------------------------------------------------
namespace AtspiApi {
namespace {
void *g_libatspi = nullptr;
bool g_loaded = false;
}  // namespace

bool Load()
{
    if (g_loaded)
        return true;
    g_libatspi = ::dlopen("libatspi.so.0", RTLD_NOW | RTLD_GLOBAL);
    g_loaded = (g_libatspi != nullptr);
    return g_loaded;
}

bool IsLoaded()
{
    return g_loaded;
}
}  // namespace AtspiApi

// ---------------------------------------------------------------------------
namespace Atspi {
namespace {

GMainContext *g_mainContext = nullptr;
bool g_initialized = false;
bool g_ready = false;

// Session accessibility advertisement state.
bool g_sessionA11yAttempted = false;
bool g_sessionA11yAdvertised = false;
bool g_gnomeToolkitAccessibilityAttempted = false;

// Caches maintained from AT-SPI events.
AtspiAccessible *g_cachedFocus = nullptr;
AtspiAccessible *g_cachedActiveWindow = nullptr;
uint64_t g_collectionEpoch = 1;

AtspiEventListener *g_focusListener = nullptr;
AtspiEventListener *g_windowListener = nullptr;
bool g_listenersRegistered = false;

// Foreground hints.
int g_foregroundProcessId = 0;
bool g_foregroundPidFromCompositor = false;
bool g_ownProcessForeground = false;
int g_hostPid = -1;

std::chrono::steady_clock::time_point g_lastResumeRecoverAt;

void ClearGError(GError *&err)
{
    if (err != nullptr) {
        g_error_free(err);
        err = nullptr;
    }
}

void ReplaceRef(AtspiAccessible *&slot, AtspiAccessible *obj)
{
    if (slot == obj)
        return;
    if (obj != nullptr)
        g_object_ref(obj);
    if (slot != nullptr)
        g_object_unref(slot);
    slot = obj;
}

void OnFocusEvent(AtspiEvent *event, void *)
{
    if (event == nullptr)
        return;
    if (event->source == nullptr) {
        g_boxed_free(atspi_event_get_type(), event);
        return;
    }
    bool gained = true;
    if (event->type != nullptr && std::strncmp(event->type, "object:state-changed", 20) == 0)
        gained = (event->detail1 != 0);
    if (gained) {
        if (g_cachedFocus != event->source)
            ++g_collectionEpoch;
        ReplaceRef(g_cachedFocus, event->source);
    } else if (g_cachedFocus == event->source) {
        ReplaceRef(g_cachedFocus, nullptr);
    }
    g_boxed_free(atspi_event_get_type(), event);
}

void OnWindowEvent(AtspiEvent *event, void *)
{
    if (event == nullptr)
        return;
    if (event->source == nullptr) {
        g_boxed_free(atspi_event_get_type(), event);
        return;
    }
    if (event->type != nullptr && std::strcmp(event->type, "window:activate") == 0) {
        ReplaceRef(g_cachedActiveWindow, event->source);
    } else if (g_cachedActiveWindow == event->source) {
        ReplaceRef(g_cachedActiveWindow, nullptr);
    }
    g_boxed_free(atspi_event_get_type(), event);
}

void RegisterListeners()
{
    if (g_listenersRegistered)
        return;
    GError *err = nullptr;
    if (g_focusListener == nullptr)
        g_focusListener = atspi_event_listener_new(OnFocusEvent, nullptr, nullptr);
    if (g_windowListener == nullptr)
        g_windowListener = atspi_event_listener_new(OnWindowEvent, nullptr, nullptr);
    bool ok = true;
    ok &= atspi_event_listener_register(g_focusListener, "object:state-changed:focused", &err) != 0;
    ClearGError(err);
    atspi_event_listener_register(g_focusListener, "focus:", &err);
    ClearGError(err);
    ok &= atspi_event_listener_register(g_windowListener, "window:activate", &err) != 0;
    ClearGError(err);
    atspi_event_listener_register(g_windowListener, "window:deactivate", &err);
    ClearGError(err);
    g_listenersRegistered = ok;
}

void DeregisterListeners()
{
    GError *err = nullptr;
    if (g_focusListener != nullptr) {
        atspi_event_listener_deregister(g_focusListener, "object:state-changed:focused", &err);
        ClearGError(err);
        atspi_event_listener_deregister(g_focusListener, "focus:", &err);
        ClearGError(err);
    }
    if (g_windowListener != nullptr) {
        atspi_event_listener_deregister(g_windowListener, "window:activate", &err);
        ClearGError(err);
        atspi_event_listener_deregister(g_windowListener, "window:deactivate", &err);
        ClearGError(err);
    }
    g_listenersRegistered = false;
}

bool SetA11yStatusBoolean(DBusConnection *conn, const char *property, bool value)
{
    DBusMessage *msg = dbus_message_new_method_call("org.a11y.Bus", "/org/a11y/bus",
                                                    "org.freedesktop.DBus.Properties", "Set");
    if (msg == nullptr)
        return false;
    const char *iface = "org.a11y.Status";
    DBusMessageIter it, var;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &property);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &var);
    dbus_bool_t b = value ? TRUE : FALSE;
    dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
    dbus_message_iter_close_container(&it, &var);
    DBusError derr;
    dbus_error_init(&derr);
    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(conn, msg, kStartupTimeoutMs, &derr);
    dbus_message_unref(msg);
    bool ok = reply != nullptr && !dbus_error_is_set(&derr);
    if (reply != nullptr)
        dbus_message_unref(reply);
    dbus_error_free(&derr);
    return ok;
}

void EnableGnomeToolkitAccessibility()
{
    if (g_gnomeToolkitAccessibilityAttempted)
        return;
    g_gnomeToolkitAccessibilityAttempted = true;
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    if (src == nullptr)
        return;
    GSettingsSchema *schema =
        g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE);
    if (schema == nullptr)
        return;
    if (g_settings_schema_has_key(schema, "toolkit-accessibility")) {
        GSettings *settings = g_settings_new_full(schema, nullptr, nullptr);
        if (settings != nullptr) {
            if (!g_settings_get_boolean(settings, "toolkit-accessibility")) {
                g_settings_set_boolean(settings, "toolkit-accessibility", TRUE);
                g_settings_sync();
            }
            g_object_unref(settings);
        }
    }
    g_settings_schema_unref(schema);
}

}  // namespace

// ---------------------------------------------------------------------------
Deadline::Deadline(int timeoutMs)
    : end_(std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs))),
      infinite_(timeoutMs < 0)
{
}

bool Deadline::Expired() const
{
    return !infinite_ && std::chrono::steady_clock::now() >= end_;
}

int Deadline::RemainingMs() const
{
    if (infinite_)
        return 1 << 30;
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    end_ - std::chrono::steady_clock::now())
                    .count();
    return left > 0 ? static_cast<int>(left) : 0;
}

bool EnsureInitialized()
{
    if (g_initialized)
        return g_ready;
    g_initialized = true;
    if (!AtspiApi::Load())
        return false;
    g_mainContext = g_main_context_new();
    g_main_context_push_thread_default(g_mainContext);
    atspi_set_timeout(kMethodTimeoutMs, kStartupTimeoutMs);
    int rc = atspi_init();
    g_ready = (rc == 0 || rc == 1);
    if (!g_ready)
        return false;
    atspi_set_main_context(g_mainContext);
    if (const char *host = std::getenv("TYPELESS_A11Y_HOST_PID"))
        g_hostPid = std::atoi(host);
    RegisterListeners();
    return true;
}

bool IsReady()
{
    return g_ready;
}

bool EnableSessionAccessibility(bool reassert)
{
    if (g_sessionA11yAttempted && !reassert)
        return g_sessionA11yAdvertised;
    g_sessionA11yAttempted = true;
    DBusError derr;
    dbus_error_init(&derr);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &derr);
    dbus_error_free(&derr);
    if (conn != nullptr) {
        dbus_connection_set_exit_on_disconnect(conn, FALSE);
        if (SetA11yStatusBoolean(conn, "IsEnabled", true))
            g_sessionA11yAdvertised = true;
        dbus_connection_unref(conn);
    }
    EnableGnomeToolkitAccessibility();
    return g_sessionA11yAdvertised;
}

bool IsDesktopReachable()
{
    if (!EnsureInitialized())
        return false;
    AtspiAccessible *desktop = atspi_get_desktop(0);
    if (desktop == nullptr)
        return false;
    GError *err = nullptr;
    int n = atspi_accessible_get_child_count(desktop, &err);
    bool ok = (err == nullptr && n >= 0);
    ClearGError(err);
    g_object_unref(desktop);
    return ok;
}

bool EnsureAccessibleReady()
{
    if (!EnsureInitialized())
        return false;
    RegisterListeners();
    return g_ready;
}

void PumpEvents()
{
    if (g_mainContext == nullptr)
        return;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kPumpEventsMaxIterations; ++i) {
        if (!g_main_context_pending(g_mainContext))
            break;
        g_main_context_iteration(g_mainContext, FALSE);
        if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(kPumpEventsMaxMs))
            break;
    }
}

void DrainPendingCacheUpdates()
{
    PumpEvents();
    if (g_cachedFocus != nullptr && IsDefunctAccessible(g_cachedFocus))
        ReplaceRef(g_cachedFocus, nullptr);
    if (g_cachedActiveWindow != nullptr && IsDefunctAccessible(g_cachedActiveWindow))
        ReplaceRef(g_cachedActiveWindow, nullptr);
}

uint64_t CollectionCacheEpoch()
{
    return g_collectionEpoch;
}

void ReleaseAccessibleCaches()
{
    ReplaceRef(g_cachedFocus, nullptr);
    ReplaceRef(g_cachedActiveWindow, nullptr);
    ++g_collectionEpoch;
}

void RecoverAfterResume()
{
    g_lastResumeRecoverAt = std::chrono::steady_clock::now();
    ReleaseAccessibleCaches();
    if (g_ready) {
        DeregisterListeners();
        RegisterListeners();
    }
}

void NoteForegroundProcessId(int pid)
{
    g_foregroundProcessId = pid > 0 ? pid : 0;
    g_foregroundPidFromCompositor = pid > 0;
    g_ownProcessForeground = false;
}

void NoteOwnProcessForeground()
{
    g_ownProcessForeground = true;
}

void ReleaseCompositorForegroundLock()
{
    g_foregroundPidFromCompositor = false;
    g_foregroundProcessId = 0;
    g_ownProcessForeground = false;
}

int ForegroundProcessId()
{
    return g_foregroundProcessId;
}

Session::Session() : ok_(EnsureInitialized()) {}
Session::~Session() = default;

// ---------------------------------------------------------------------------
bool IsLiveAtspiAccessible(AtspiAccessible *obj)
{
    return obj != nullptr && ATSPI_IS_ACCESSIBLE(obj);
}

bool IsDefunctAccessible(AtspiAccessible *obj)
{
    if (!IsLiveAtspiAccessible(obj))
        return true;
    AtspiStateSet *set = atspi_accessible_get_state_set(obj);
    if (set == nullptr)
        return true;
    bool defunct = atspi_state_set_contains(set, ATSPI_STATE_DEFUNCT);
    g_object_unref(set);
    return defunct;
}

AtspiRole GetRole(AtspiAccessible *obj)
{
    if (!IsLiveAtspiAccessible(obj))
        return ATSPI_ROLE_INVALID;
    GError *err = nullptr;
    AtspiRole role = atspi_accessible_get_role(obj, &err);
    if (err != nullptr) {
        ClearGError(err);
        return ATSPI_ROLE_INVALID;
    }
    return role;
}

std::string GetName(AtspiAccessible *obj)
{
    if (!IsLiveAtspiAccessible(obj))
        return std::string();
    GError *err = nullptr;
    gchar *s = atspi_accessible_get_name(obj, &err);
    ClearGError(err);
    std::string out = s ? s : "";
    g_free(s);
    return out;
}

std::string GetDescription(AtspiAccessible *obj)
{
    if (!IsLiveAtspiAccessible(obj))
        return std::string();
    GError *err = nullptr;
    gchar *s = atspi_accessible_get_description(obj, &err);
    ClearGError(err);
    std::string out = s ? s : "";
    g_free(s);
    return out;
}

AtspiStateSet *GetStateSet(AtspiAccessible *obj)
{
    if (!IsLiveAtspiAccessible(obj))
        return nullptr;
    return atspi_accessible_get_state_set(obj);
}

bool StateSetContains(AtspiStateSet *set, AtspiStateType state)
{
    return set != nullptr && atspi_state_set_contains(set, state);
}

bool HasState(AtspiAccessible *obj, AtspiStateType state)
{
    AtspiStateSet *set = GetStateSet(obj);
    bool r = StateSetContains(set, state);
    if (set != nullptr)
        g_object_unref(set);
    return r;
}

int CachedProcessId(AtspiAccessible *obj)
{
    if (!IsLiveAtspiAccessible(obj))
        return 0;
    GError *err = nullptr;
    guint pid = atspi_accessible_get_process_id(obj, &err);
    if (err != nullptr) {
        ClearGError(err);
        return 0;
    }
    return static_cast<int>(pid);
}

AtspiAccessible *GetParent(AtspiAccessible *obj)
{
    if (!IsLiveAtspiAccessible(obj))
        return nullptr;
    GError *err = nullptr;
    AtspiAccessible *p = atspi_accessible_get_parent(obj, &err);
    ClearGError(err);
    return p;
}

int GetChildCount(AtspiAccessible *obj)
{
    if (!IsLiveAtspiAccessible(obj))
        return 0;
    GError *err = nullptr;
    int n = atspi_accessible_get_child_count(obj, &err);
    if (err != nullptr) {
        ClearGError(err);
        return 0;
    }
    return n < 0 ? 0 : n;
}

AtspiAccessible *GetChildAtIndex(AtspiAccessible *obj, int index)
{
    if (!IsLiveAtspiAccessible(obj))
        return nullptr;
    GError *err = nullptr;
    AtspiAccessible *c = atspi_accessible_get_child_at_index(obj, index, &err);
    ClearGError(err);
    return c;
}

AtspiText *GetTextInterface(AtspiAccessible *obj)
{
    if (!IsLiveAtspiAccessible(obj))
        return nullptr;
    return atspi_accessible_get_text_iface(obj);
}

bool HasEditableTextInterface(AtspiAccessible *obj)
{
    if (!IsLiveAtspiAccessible(obj))
        return false;
    AtspiEditableText *e = atspi_accessible_get_editable_text_iface(obj);
    if (e == nullptr)
        return false;
    g_object_unref(e);
    return true;
}

int GetTextCharacterCount(AtspiText *text)
{
    if (text == nullptr)
        return -1;
    GError *err = nullptr;
    int n = atspi_text_get_character_count(text, &err);
    if (err != nullptr) {
        ClearGError(err);
        return -1;
    }
    return n;
}

int GetTextCaretOffset(AtspiText *text)
{
    if (text == nullptr)
        return -1;
    GError *err = nullptr;
    int n = atspi_text_get_caret_offset(text, &err);
    if (err != nullptr) {
        ClearGError(err);
        return -1;
    }
    return n;
}

bool ReadAccessibleText(AtspiText *text, int start, int end, int maxCharsPerCall,
                        std::string &out)
{
    out.clear();
    if (text == nullptr || start < 0 || end < start)
        return false;
    if (maxCharsPerCall <= 0)
        maxCharsPerCall = end - start;
    for (int pos = start; pos < end;) {
        int stop = std::min(end, pos + std::max(1, maxCharsPerCall));
        GError *err = nullptr;
        gchar *s = atspi_text_get_text(text, pos, stop, &err);
        if (err != nullptr) {
            ClearGError(err);
            g_free(s);
            return false;
        }
        if (s != nullptr) {
            out += s;
            g_free(s);
        }
        pos = stop;
    }
    return true;
}

void ReadAccessibleFocusKey(AtspiAccessible *obj, std::string &busName, std::string &objectPath)
{
    busName.clear();
    objectPath.clear();
    if (!IsLiveAtspiAccessible(obj))
        return;
    // AtspiAccessible derives from AtspiObject {GObject; AtspiApplication *app; char *path}.
    AtspiObject *o = reinterpret_cast<AtspiObject *>(obj);
    if (o->app != nullptr && o->app->bus_name != nullptr)
        busName = o->app->bus_name;
    if (o->path != nullptr)
        objectPath = o->path;
}

bool IsEditableFromSnapshot(AtspiStateSet *set, AtspiRole role)
{
    if (set == nullptr)
        return false;
    if (atspi_state_set_contains(set, ATSPI_STATE_READ_ONLY))
        return false;
    if (atspi_state_set_contains(set, ATSPI_STATE_EDITABLE))
        return true;
    return role == ATSPI_ROLE_PASSWORD_TEXT;
}

bool IsExplicitlyNonTextControl(AtspiAccessible *obj)
{
    switch (GetRole(obj)) {
    case ATSPI_ROLE_PUSH_BUTTON:
    case ATSPI_ROLE_TOGGLE_BUTTON:
    case ATSPI_ROLE_CHECK_BOX:
    case ATSPI_ROLE_RADIO_BUTTON:
    case ATSPI_ROLE_MENU_ITEM:
    case ATSPI_ROLE_CHECK_MENU_ITEM:
    case ATSPI_ROLE_RADIO_MENU_ITEM:
    case ATSPI_ROLE_MENU:
    case ATSPI_ROLE_PAGE_TAB:
    case ATSPI_ROLE_SLIDER:
    case ATSPI_ROLE_SCROLL_BAR:
    case ATSPI_ROLE_LINK:
    case ATSPI_ROLE_LIST_ITEM:
    case ATSPI_ROLE_TREE_ITEM:
        return true;
    default:
        return false;
    }
}

bool IsOwnProcessId(int pid)
{
    if (pid <= 0)
        return false;
    return pid == static_cast<int>(::getpid()) || (g_hostPid > 0 && pid == g_hostPid);
}

bool VisitChildren(AtspiAccessible *root, const TreeWalkLimits &limits,
                   const std::function<bool()> &shouldAbort,
                   const std::function<CollectionWalkStatus(AtspiAccessible *)> &visit)
{
    if (!IsLiveAtspiAccessible(root))
        return false;
    int maxDepth = limits.maxDepth > 0 ? limits.maxDepth : 32;
    int maxBreadth = limits.maxBreadth > 0 ? limits.maxBreadth : 256;
    int maxNodes = limits.maxNodes > 0 ? limits.maxNodes : 4000;
    struct Item {
        AtspiAccessible *obj;
        int depth;
    };
    std::deque<Item> queue;
    g_object_ref(root);
    queue.push_back({root, 0});
    int visited = 0;
    bool completed = true;
    while (!queue.empty()) {
        Item it = queue.front();
        queue.pop_front();
        if ((shouldAbort && shouldAbort()) || visited >= maxNodes) {
            g_object_unref(it.obj);
            completed = false;
            break;
        }
        bool descend = true;
        if (it.depth > 0) {
            ++visited;
            CollectionWalkStatus st = visit(it.obj);
            if (st == CollectionWalkStatus::kStop) {
                g_object_unref(it.obj);
                completed = false;
                break;
            }
            descend = (st != CollectionWalkStatus::kSkipChildren);
        }
        if (descend && it.depth < maxDepth) {
            int n = std::min(GetChildCount(it.obj), maxBreadth);
            for (int i = 0; i < n; ++i) {
                AtspiAccessible *c = GetChildAtIndex(it.obj, i);
                if (c != nullptr)
                    queue.push_back({c, it.depth + 1});
            }
        }
        g_object_unref(it.obj);
    }
    for (auto &rest : queue)
        g_object_unref(rest.obj);
    return completed;
}

namespace {

bool IsTopLevelRole(AtspiRole role)
{
    return role == ATSPI_ROLE_FRAME || role == ATSPI_ROLE_WINDOW || role == ATSPI_ROLE_DIALOG ||
           role == ATSPI_ROLE_FILE_CHOOSER || role == ATSPI_ROLE_ALERT;
}

// Active window of one application (new ref) or nullptr.
AtspiAccessible *ActiveWindowOfApp(AtspiAccessible *app, bool fallbackShowing)
{
    int n = std::min(GetChildCount(app), 64);
    AtspiAccessible *showing = nullptr;
    for (int i = 0; i < n; ++i) {
        AtspiAccessible *w = GetChildAtIndex(app, i);
        if (w == nullptr)
            continue;
        AtspiStateSet *set = GetStateSet(w);
        bool active = StateSetContains(set, ATSPI_STATE_ACTIVE);
        bool isShowing = StateSetContains(set, ATSPI_STATE_SHOWING);
        if (set)
            g_object_unref(set);
        if (active) {
            if (showing)
                g_object_unref(showing);
            return w;
        }
        if (fallbackShowing && isShowing && showing == nullptr)
            showing = w;
        else
            g_object_unref(w);
    }
    return showing;
}

}  // namespace

AtspiAccessible *FindActiveWindow()
{
    if (!EnsureInitialized())
        return nullptr;
    // Cached from window:activate events.
    if (g_cachedActiveWindow != nullptr && !IsDefunctAccessible(g_cachedActiveWindow) &&
        HasState(g_cachedActiveWindow, ATSPI_STATE_ACTIVE)) {
        int pid = CachedProcessId(g_cachedActiveWindow);
        if (g_foregroundProcessId <= 0 || pid == g_foregroundProcessId) {
            g_object_ref(g_cachedActiveWindow);
            return g_cachedActiveWindow;
        }
    }
    AtspiAccessible *desktop = atspi_get_desktop(0);
    if (desktop == nullptr)
        return nullptr;
    int n = GetChildCount(desktop);
    AtspiAccessible *found = nullptr;
    AtspiAccessible *foregroundFallback = nullptr;
    AtspiAccessible *ownFallback = nullptr;
    for (int i = 0; i < n && found == nullptr; ++i) {
        AtspiAccessible *app = GetChildAtIndex(desktop, i);
        if (app == nullptr)
            continue;
        int pid = CachedProcessId(app);
        bool isForeground = g_foregroundProcessId > 0 && pid == g_foregroundProcessId;
        AtspiAccessible *w = ActiveWindowOfApp(app, isForeground);
        if (w != nullptr) {
            AtspiStateSet *set = GetStateSet(w);
            bool active = StateSetContains(set, ATSPI_STATE_ACTIVE);
            if (set)
                g_object_unref(set);
            if (IsOwnProcessId(pid) && !isForeground) {
                if (ownFallback == nullptr && active)
                    ownFallback = w;
                else
                    g_object_unref(w);
            } else if (isForeground && !active) {
                if (foregroundFallback == nullptr)
                    foregroundFallback = w;
                else
                    g_object_unref(w);
            } else if (g_foregroundProcessId > 0 && !isForeground) {
                // Another app claims ACTIVE while the compositor told us who is
                // in front; keep it only as a last resort.
                if (ownFallback == nullptr)
                    ownFallback = w;
                else
                    g_object_unref(w);
            } else {
                found = w;
            }
        }
        g_object_unref(app);
    }
    g_object_unref(desktop);
    if (found == nullptr && foregroundFallback != nullptr) {
        found = foregroundFallback;
        foregroundFallback = nullptr;
    }
    if (found == nullptr && ownFallback != nullptr) {
        found = ownFallback;
        ownFallback = nullptr;
    }
    if (foregroundFallback)
        g_object_unref(foregroundFallback);
    if (ownFallback)
        g_object_unref(ownFallback);
    if (found != nullptr)
        ReplaceRef(g_cachedActiveWindow, found);
    return found;
}

AtspiAccessible *FindTopLevelWindowOf(AtspiAccessible *obj, int maxLevels)
{
    if (!IsLiveAtspiAccessible(obj))
        return nullptr;
    if (maxLevels <= 0)
        maxLevels = 64;
    g_object_ref(obj);
    AtspiAccessible *cur = obj;
    for (int i = 0; i < maxLevels && cur != nullptr; ++i) {
        AtspiAccessible *parent = GetParent(cur);
        if (parent == nullptr)
            break;
        if (GetRole(parent) == ATSPI_ROLE_APPLICATION ||
            GetRole(parent) == ATSPI_ROLE_DESKTOP_FRAME) {
            g_object_unref(parent);
            return cur;  // cur is the top-level child of the application
        }
        g_object_unref(cur);
        cur = parent;
    }
    if (cur != nullptr && IsTopLevelRole(GetRole(cur)))
        return cur;
    if (cur)
        g_object_unref(cur);
    return nullptr;
}

namespace {

// Focused descendant via the Collection interface (one D-Bus round trip).
AtspiAccessible *FindFocusedViaCollection(AtspiAccessible *window)
{
    AtspiCollection *coll = atspi_accessible_get_collection_iface(window);
    if (coll == nullptr)
        return nullptr;
    AtspiStateSet *states = atspi_state_set_new(nullptr);
    atspi_state_set_add(states, ATSPI_STATE_FOCUSED);
    AtspiMatchRule *rule = atspi_match_rule_new(states, ATSPI_Collection_MATCH_ALL, nullptr,
                                                ATSPI_Collection_MATCH_NONE, nullptr,
                                                ATSPI_Collection_MATCH_NONE, nullptr,
                                                ATSPI_Collection_MATCH_NONE, FALSE);
    GError *err = nullptr;
    GArray *matches = atspi_collection_get_matches(coll, rule, ATSPI_Collection_SORT_ORDER_CANONICAL,
                                                   1, TRUE, &err);
    AtspiAccessible *found = nullptr;
    if (err == nullptr && matches != nullptr && matches->len > 0) {
        found = g_array_index(matches, AtspiAccessible *, 0);
        if (found)
            g_object_ref(found);
    }
    ClearGError(err);
    if (matches != nullptr) {
        for (guint i = 0; i < matches->len; ++i) {
            AtspiAccessible *m = g_array_index(matches, AtspiAccessible *, i);
            if (m)
                g_object_unref(m);
        }
        g_array_free(matches, TRUE);
    }
    g_object_unref(rule);
    g_object_unref(states);
    g_object_unref(coll);
    return found;
}

AtspiAccessible *FindFocusedViaWalk(AtspiAccessible *window, const TreeWalkLimits &limits,
                                    const Deadline &deadline)
{
    AtspiAccessible *found = nullptr;
    VisitChildren(
        window, limits, [&] { return deadline.Expired(); },
        [&](AtspiAccessible *node) {
            AtspiStateSet *set = GetStateSet(node);
            bool focused = StateSetContains(set, ATSPI_STATE_FOCUSED);
            bool showing = set == nullptr || StateSetContains(set, ATSPI_STATE_SHOWING);
            bool manages = StateSetContains(set, ATSPI_STATE_MANAGES_DESCENDANTS);
            if (set)
                g_object_unref(set);
            if (focused) {
                found = node;
                g_object_ref(found);
                return CollectionWalkStatus::kStop;
            }
            if (!showing || manages)
                return CollectionWalkStatus::kSkipChildren;
            return CollectionWalkStatus::kContinue;
        });
    return found;
}

}  // namespace

AtspiAccessible *GetFocusedAccessible(int timeoutMs, const TreeWalkLimits &limits)
{
    if (!EnsureInitialized())
        return nullptr;
    Deadline deadline(timeoutMs > 0 ? timeoutMs : 1000);
    // 1. Event-driven cache.
    if (g_cachedFocus != nullptr) {
        AtspiStateSet *set = GetStateSet(g_cachedFocus);
        bool usable = set != nullptr && !StateSetContains(set, ATSPI_STATE_DEFUNCT) &&
                      StateSetContains(set, ATSPI_STATE_FOCUSED);
        if (set)
            g_object_unref(set);
        if (usable) {
            int pid = CachedProcessId(g_cachedFocus);
            if (g_foregroundProcessId <= 0 || pid == g_foregroundProcessId || pid == 0) {
                g_object_ref(g_cachedFocus);
                return g_cachedFocus;
            }
        } else {
            ReplaceRef(g_cachedFocus, nullptr);
        }
    }
    // 2. Scan the active window.
    AtspiAccessible *window = FindActiveWindow();
    if (window == nullptr)
        return nullptr;
    AtspiAccessible *focused = nullptr;
    if (HasState(window, ATSPI_STATE_FOCUSED)) {
        focused = window;
        g_object_ref(focused);
    }
    if (focused == nullptr && !deadline.Expired())
        focused = FindFocusedViaCollection(window);
    if (focused == nullptr && !deadline.Expired())
        focused = FindFocusedViaWalk(window, limits, deadline);
    g_object_unref(window);
    if (focused != nullptr)
        ReplaceRef(g_cachedFocus, focused);
    return focused;
}

namespace {

bool IsEditableNode(AtspiAccessible *obj, AtspiRole *roleOut = nullptr)
{
    AtspiRole role = GetRole(obj);
    if (roleOut)
        *roleOut = role;
    AtspiStateSet *set = GetStateSet(obj);
    bool editable = IsEditableFromSnapshot(set, role);
    if (set)
        g_object_unref(set);
    return editable;
}

}  // namespace

AtspiAccessible *LookupFocusedEditable(const char *tag, std::string *error,
                                       const EditableSearchLimits &limits, int timeoutMs)
{
    (void)tag;
    Deadline deadline(timeoutMs > 0 ? timeoutMs : 1000);
    TreeWalkLimits walk;
    AtspiAccessible *focus = GetFocusedAccessible(deadline.RemainingMs(), walk);
    if (focus == nullptr) {
        if (error)
            *error = deadline.Expired() ? "timeout" : "no_focused_element";
        return nullptr;
    }
    if (IsEditableNode(focus))
        return focus;

    // Ancestors (e.g. a focused paragraph inside a contenteditable host).
    int maxAncestors = limits.maxAncestorLevels > 0 ? limits.maxAncestorLevels : 8;
    AtspiAccessible *cur = focus;
    g_object_ref(cur);
    for (int i = 0; i < maxAncestors && !deadline.Expired(); ++i) {
        AtspiAccessible *parent = GetParent(cur);
        g_object_unref(cur);
        cur = parent;
        if (cur == nullptr)
            break;
        AtspiRole role = GetRole(cur);
        if (role == ATSPI_ROLE_APPLICATION || role == ATSPI_ROLE_FRAME ||
            role == ATSPI_ROLE_WINDOW || role == ATSPI_ROLE_DESKTOP_FRAME)
            break;
        if (IsEditableNode(cur)) {
            g_object_unref(focus);
            return cur;
        }
    }
    if (cur)
        g_object_unref(cur);

    // Descendants (e.g. focus on a composite widget wrapping the entry).
    AtspiAccessible *found = nullptr;
    if (!IsExplicitlyNonTextControl(focus)) {
        TreeWalkLimits dl;
        dl.maxDepth = 16;
        dl.maxBreadth = limits.maxBreadth > 0 ? limits.maxBreadth : 100;
        dl.maxNodes = limits.maxDescendantNodes > 0 ? limits.maxDescendantNodes : 400;
        AtspiAccessible *firstEditable = nullptr;
        VisitChildren(
            focus, dl, [&] { return deadline.Expired(); },
            [&](AtspiAccessible *node) {
                AtspiStateSet *set = GetStateSet(node);
                AtspiRole role = GetRole(node);
                bool editable = IsEditableFromSnapshot(set, role);
                bool focused = StateSetContains(set, ATSPI_STATE_FOCUSED);
                if (set)
                    g_object_unref(set);
                if (editable && focused) {
                    found = node;
                    g_object_ref(found);
                    return CollectionWalkStatus::kStop;
                }
                if (editable && firstEditable == nullptr) {
                    firstEditable = node;
                    g_object_ref(firstEditable);
                }
                return CollectionWalkStatus::kContinue;
            });
        AtspiRole focusRole = GetRole(focus);
        bool wrapperRole = focusRole == ATSPI_ROLE_COMBO_BOX || focusRole == ATSPI_ROLE_SPIN_BUTTON ||
                           focusRole == ATSPI_ROLE_AUTOCOMPLETE || focusRole == ATSPI_ROLE_EDITBAR;
        if (found == nullptr && wrapperRole) {
            found = firstEditable;
        } else if (firstEditable != nullptr) {
            g_object_unref(firstEditable);
        }
    }
    g_object_unref(focus);
    if (found == nullptr && error)
        *error = deadline.Expired() ? "timeout" : "not_editable";
    return found;
}

FocusedInputState ReadFocusedInputState(const EditableSearchLimits &limits)
{
    FocusedInputState st;
    if (!EnsureInitialized()) {
        st.error = "atspi_initialization_failed";
        return st;
    }
    const int timeoutMs = 1500;
    Deadline deadline(timeoutMs);
    std::string error;
    AtspiAccessible *editable = LookupFocusedEditable("ReadFocusedInputState", &error, limits,
                                                      timeoutMs);
    if (editable == nullptr) {
        st.error = error.empty() ? "no_focused_element" : error;
        return st;
    }
    if (GetRole(editable) == ATSPI_ROLE_PASSWORD_TEXT) {
        g_object_unref(editable);
        st.error = "password_field";
        return st;
    }
    uint64_t epochBefore = g_collectionEpoch;
    AtspiText *text = GetTextInterface(editable);
    if (text == nullptr) {
        g_object_unref(editable);
        st.error = "no_text_interface";
        return st;
    }
    int count = GetTextCharacterCount(text);
    if (count < 0) {
        g_object_unref(text);
        g_object_unref(editable);
        st.error = "no_text_interface";
        return st;
    }
    int caret = GetTextCaretOffset(text);
    if (caret < 0 || caret > count)
        caret = std::clamp(caret, 0, count);
    int selStart = caret, selEnd = caret;
    GError *err = nullptr;
    int nsel = atspi_text_get_n_selections(text, &err);
    ClearGError(err);
    if (nsel > 0) {
        AtspiRange *r = atspi_text_get_selection(text, 0, &err);
        ClearGError(err);
        if (r != nullptr) {
            int a = std::clamp(std::min(r->start_offset, r->end_offset), 0, count);
            int b = std::clamp(std::max(r->start_offset, r->end_offset), 0, count);
            if (b > a) {
                selStart = a;
                selEnd = b;
            }
            g_free(r);
        }
    }

    int maxLen = limits.maxInputTextLength > 0 ? limits.maxInputTextLength : 5000;
    int beforeStart = 0, afterEnd = count;
    int selectedEnd = selEnd;
    if (count > maxLen) {
        int selLen = std::min(selEnd - selStart, maxLen);
        selectedEnd = selStart + selLen;
        int budget = maxLen - selLen;
        int beforeLen = std::min(selStart, budget / 2);
        int afterLen = std::min(count - selEnd, budget - beforeLen);
        beforeLen = std::min(selStart, budget - afterLen);
        beforeStart = selStart - beforeLen;
        afterEnd = selEnd + afterLen;
    }
    const int chunk = 4096;
    bool ok = ReadAccessibleText(text, beforeStart, selStart, chunk, st.beforeText) &&
              ReadAccessibleText(text, selStart, selectedEnd, chunk, st.selectedText) &&
              ReadAccessibleText(text, selEnd, afterEnd, chunk, st.afterText);
    g_object_unref(text);
    g_object_unref(editable);
    if (!ok) {
        st.beforeText.clear();
        st.selectedText.clear();
        st.afterText.clear();
        st.error = deadline.Expired() ? "timeout" : "no_text_interface";
        return st;
    }
    if (deadline.Expired()) {
        st.beforeText.clear();
        st.selectedText.clear();
        st.afterText.clear();
        st.error = "timeout";
        return st;
    }
    (void)epochBefore;
    st.valid = true;
    return st;
}

FocusedInputCapability ProbeFocusedInputCapability(const EditableSearchLimits &limits)
{
    if (!EnsureInitialized())
        return FocusedInputCapability::kUnknown;
    std::string error;
    AtspiAccessible *editable =
        LookupFocusedEditable("ProbeFocusedInputCapability", &error, limits, 800);
    if (editable != nullptr) {
        g_object_unref(editable);
        return FocusedInputCapability::kEditable;
    }
    if (error == "not_editable")
        return FocusedInputCapability::kNotEditable;
    return FocusedInputCapability::kUnknown;
}

}  // namespace Atspi
}  // namespace LinuxShared
