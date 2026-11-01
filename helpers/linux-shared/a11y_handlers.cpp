// a11y_handlers.cpp - command dispatch and payload builders for
// typeless-atspi-helper. Interoperable reimplementation.
#include "a11y_handlers.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string>

#include "a11y_ipc.h"
#include "atspi_accessible.h"

// at-spi2-core before 2.54 has no ATSPI_ROLE_SWITCH. The last concrete role in
// those headers is what this "does this role have a name" check wants.
#ifndef ATSPI_ROLE_SWITCH
#define ATSPI_ROLE_SWITCH (ATSPI_ROLE_LAST_DEFINED - 1)
#endif

namespace LinuxShared {
namespace A11y {
namespace {

using namespace LinuxShared::Atspi;

const char *Bool(bool b)
{
    return b ? "true" : "false";
}

std::string Str(const std::string &s)
{
    return "\"" + EscapeJsonString(s) + "\"";
}

A11yResponse MakeOk(const A11yRequest &request, std::string payload)
{
    A11yResponse r;
    r.id = request.id;
    r.ok = true;
    r.error = A11yError::kOk;
    r.generation = CollectionCacheEpoch();
    r.payloadJson = std::move(payload);
    return r;
}

A11yResponse MakeFailure(const A11yRequest &request, A11yError error, const char *reason,
                         std::string payload = "{}")
{
    A11yResponse r;
    r.id = request.id;
    r.ok = false;
    r.error = error;
    r.failReason = reason;
    r.generation = CollectionCacheEpoch();
    r.payloadJson = std::move(payload);
    return r;
}

EditableSearchLimits EffectiveEditableLimits(const A11yRequest &r)
{
    EditableSearchLimits l;
    l.maxAncestorLevels = r.editableSearchLimits.maxAncestorLevels > 0
                              ? r.editableSearchLimits.maxAncestorLevels : 8;
    l.maxDescendantNodes = r.editableSearchLimits.maxDescendantNodes > 0
                               ? r.editableSearchLimits.maxDescendantNodes : 400;
    l.maxBreadth = r.editableSearchLimits.maxBreadth > 0 ? r.editableSearchLimits.maxBreadth : 100;
    l.maxInputTextLength = r.editableSearchLimits.maxInputTextLength > 0
                               ? r.editableSearchLimits.maxInputTextLength : 5000;
    return l;
}

std::string AppNameOf(AtspiAccessible *obj)
{
    if (!IsLiveAtspiAccessible(obj))
        return std::string();
    GError *err = nullptr;
    AtspiAccessible *app = atspi_accessible_get_application(obj, &err);
    if (err)
        g_error_free(err);
    if (app == nullptr)
        return std::string();
    std::string name = GetName(app);
    g_object_unref(app);
    return name;
}

// ---- GetActiveWindowSnapshot ----------------------------------------------
// Payload (x64 key order): {"expired":b,"processId":n,"foregroundProcessId":n,
// "appName":"..","windowTitle":"..","hasBounds":false,"isVisible":b,
// "x":0,"y":0,"width":0,"height":0}. The original never reports bounds.
std::string ApplicationNameOf(AtspiAccessible *obj)
{
    // Walk up to the ATSPI_ROLE_APPLICATION ancestor and take its name.
    if (!IsLiveAtspiAccessible(obj))
        return std::string();
    g_object_ref(obj);
    AtspiAccessible *cur = obj;
    for (int i = 0; i < 64 && cur != nullptr; ++i) {
        if (GetRole(cur) == ATSPI_ROLE_APPLICATION) {
            std::string name = GetName(cur);
            g_object_unref(cur);
            return name;
        }
        AtspiAccessible *parent = GetParent(cur);
        g_object_unref(cur);
        cur = parent;
    }
    if (cur)
        g_object_unref(cur);
    return AppNameOf(obj);
}

std::string HandleActiveWindowSnapshot(const A11yRequest &request)
{
    (void)request;
    int fg = ForegroundProcessId();
    AtspiAccessible *active = FindActiveWindow();
    AtspiAccessible *window = nullptr;
    if (active != nullptr) {
        window = FindTopLevelWindowOf(active, 64);
        if (window == nullptr) {
            window = active;
            g_object_ref(window);
        }
        g_object_unref(active);
    }
    if (window == nullptr || IsDefunctAccessible(window)) {
        if (window)
            g_object_unref(window);
        return std::string("{\"expired\":true,\"processId\":0,\"foregroundProcessId\":") +
               std::to_string(fg) +
               ",\"appName\":\"\",\"windowTitle\":\"\",\"hasBounds\":false,\"isVisible\":true,"
               "\"x\":0,\"y\":0,\"width\":0,\"height\":0}";
    }
    int pid = CachedProcessId(window);
    AtspiStateSet *set = GetStateSet(window);
    bool defunct = set == nullptr || StateSetContains(set, ATSPI_STATE_DEFUNCT);
    bool showing = StateSetContains(set, ATSPI_STATE_SHOWING);
    if (set)
        g_object_unref(set);
    std::string title = GetName(window);
    std::string appName = ApplicationNameOf(window);
    g_object_unref(window);
    std::string s = "{";
    s += std::string("\"expired\":") + Bool(defunct) + ",";
    s += "\"processId\":" + std::to_string(pid) + ",";
    s += "\"foregroundProcessId\":" + std::to_string(fg) + ",";
    s += "\"appName\":" + Str(appName) + ",";
    s += "\"windowTitle\":" + Str(title) + ",";
    s += "\"hasBounds\":false,";
    s += std::string("\"isVisible\":") + Bool(showing && !defunct) + ",";
    s += "\"x\":0,\"y\":0,\"width\":0,\"height\":0";
    s += "}";
    return s;
}

// ---- GetFocusSnapshot ------------------------------------------------------
TreeWalkLimits FocusWalkLimits(const A11yRequest &r)
{
    TreeWalkLimits l;
    l.maxDepth = r.walkLimits.maxDepth > 0 ? r.walkLimits.maxDepth : 8;
    l.maxBreadth = r.walkLimits.maxBreadth > 0 ? r.walkLimits.maxBreadth : 20;
    l.maxNodes = r.walkLimits.maxNodes > 0 ? r.walkLimits.maxNodes : 120;
    return l;
}

int FocusScanTimeout(const A11yRequest &r)
{
    return r.focusScanTimeoutMs > 0 ? r.focusScanTimeoutMs : 250;
}

// macOS-style role names the renderer expects (same table as the x64 helper).
const char *AxRoleName(AtspiRole role)
{
    switch (role) {
    case ATSPI_ROLE_CHECK_BOX: return "AXCheckBox";
    case ATSPI_ROLE_COMBO_BOX: return "AXComboBox";
    case ATSPI_ROLE_FILLER:
    case ATSPI_ROLE_PANEL:
    case ATSPI_ROLE_SECTION:
    case ATSPI_ROLE_GROUPING: return "AXGroup";
    case ATSPI_ROLE_IMAGE: return "AXImage";
    case ATSPI_ROLE_LIST: return "AXList";
    case ATSPI_ROLE_LIST_ITEM: return "AXListItem";
    case ATSPI_ROLE_PASSWORD_TEXT:
    case ATSPI_ROLE_ENTRY: return "AXTextField";
    case ATSPI_ROLE_PUSH_BUTTON: return "AXButton";
    case ATSPI_ROLE_RADIO_BUTTON: return "AXRadioButton";
    case ATSPI_ROLE_LINK: return "AXLink";
    case ATSPI_ROLE_TERMINAL:
    case ATSPI_ROLE_TEXT:
    case ATSPI_ROLE_EDITBAR:
    case ATSPI_ROLE_DOCUMENT_FRAME:
    case ATSPI_ROLE_DOCUMENT_TEXT:
    case ATSPI_ROLE_DOCUMENT_WEB:
    case ATSPI_ROLE_DOCUMENT_EMAIL: return "AXTextArea";
    default: return "AXUnknown";
    }
}

// Payload: {"role":"AX..","subrole":"<atspi role name>","label":"<name>",
// "title":"<name>","help":"<description>","editable":b,"focused":b,
// "enabled":b,"multiline":b,"busName":"..","objectPath":"..","explicitNonText":b}
// Falls back to the active window when nothing is focused; all-default
// values when neither exists.
std::string HandleFocusSnapshot(const A11yRequest &request)
{
    TreeWalkLimits limits = FocusWalkLimits(request);
    AtspiAccessible *element = GetFocusedAccessible(FocusScanTimeout(request), limits);
    if (element != nullptr && IsDefunctAccessible(element)) {
        g_object_unref(element);
        element = nullptr;
    }
    if (element == nullptr) {
        element = FindActiveWindow();
        if (element != nullptr && IsDefunctAccessible(element)) {
            g_object_unref(element);
            element = nullptr;
        }
    }
    std::string role = "AXUnknown", subrole, label, help, busName, objectPath;
    bool editable = false, focused = false, enabled = false, multiline = false;
    bool explicitNonText = false;
    if (element != nullptr) {
        explicitNonText = IsExplicitlyNonTextControl(element);
        ReadAccessibleFocusKey(element, busName, objectPath);
        AtspiStateSet *set = GetStateSet(element);
        if (set != nullptr && !StateSetContains(set, ATSPI_STATE_DEFUNCT)) {
            AtspiRole r = GetRole(element);
            if (r <= ATSPI_ROLE_SWITCH && r != ATSPI_ROLE_EXTENDED) {
                if (gchar *rn = atspi_role_get_name(r)) {
                    subrole = rn;
                    g_free(rn);
                }
            }
            role = AxRoleName(r);
            label = GetName(element);
            help = GetDescription(element);
            editable = IsEditableFromSnapshot(set, r);
            focused = StateSetContains(set, ATSPI_STATE_FOCUSED);
            enabled = StateSetContains(set, ATSPI_STATE_ENABLED);
            if (r == ATSPI_ROLE_TERMINAL || r == ATSPI_ROLE_TEXT || r == ATSPI_ROLE_DOCUMENT_TEXT ||
                r == ATSPI_ROLE_DOCUMENT_WEB)
                multiline = true;
            else
                multiline = StateSetContains(set, ATSPI_STATE_MULTI_LINE);
        }
        if (set)
            g_object_unref(set);
        g_object_unref(element);
    }
    std::string s = "{";
    s += "\"role\":" + Str(role) + ",";
    s += "\"subrole\":" + Str(subrole) + ",";
    s += "\"label\":" + Str(label) + ",";
    s += "\"title\":" + Str(label) + ",";
    s += "\"help\":" + Str(help) + ",";
    s += std::string("\"editable\":") + Bool(editable) + ",";
    s += std::string("\"focused\":") + Bool(focused) + ",";
    s += std::string("\"enabled\":") + Bool(enabled) + ",";
    s += std::string("\"multiline\":") + Bool(multiline) + ",";
    s += "\"busName\":" + Str(busName) + ",";
    s += "\"objectPath\":" + Str(objectPath) + ",";
    s += std::string("\"explicitNonText\":") + Bool(explicitNonText);
    s += "}";
    return s;
}

// ---- GetSurroundingText ----------------------------------------------------
// Errors (dispatcher: ok:false, failReason "surrounding_text_failed"):
//   no focus           kUnavailable {"error":"Failed to get focused element"}
//   focus key/epoch    kDefunct     {"error":"Focus changed"}
//   no text element    kUnavailable {"error":"No readable text on focused element"}
//   no text interface  kUnavailable {"error":"No text interface"}
//   element went away  kDefunct     {"error":"Text collection changed"}
//   deadline hit       kAborted     {"error":"Text collection changed"}
std::string HandleSurroundingText(const A11yRequest &request, A11yError &error)
{
    error = A11yError::kOk;
    Deadline deadline(request.timeoutMs);
    TreeWalkLimits limits = FocusWalkLimits(request);
    AtspiAccessible *focus = GetFocusedAccessible(FocusScanTimeout(request), limits);
    if (focus == nullptr) {
        error = A11yError::kUnavailable;
        return "{\"error\":\"Failed to get focused element\"}";
    }
    std::string busName, objectPath;
    ReadAccessibleFocusKey(focus, busName, objectPath);
    bool keyMismatch = (request.generation != 0 && request.generation != CollectionCacheEpoch()) ||
                       (!request.busName.empty() && request.busName != busName) ||
                       (!request.objectPath.empty() && request.objectPath != objectPath);
    if (keyMismatch) {
        g_object_unref(focus);
        error = A11yError::kDefunct;
        return "{\"error\":\"Focus changed\"}";
    }
    // The focused object or its nearest ancestor that implements Text.
    AtspiAccessible *element = nullptr;
    {
        AtspiAccessible *cur = focus;
        g_object_ref(cur);
        for (int i = 0; i < 8 && cur != nullptr; ++i) {
            if (IsDefunctAccessible(cur))
                break;
            AtspiText *probe = GetTextInterface(cur);
            if (probe != nullptr) {
                g_object_unref(probe);
                element = cur;
                cur = nullptr;
                break;
            }
            AtspiRole r = GetRole(cur);
            if (r == ATSPI_ROLE_APPLICATION || r == ATSPI_ROLE_FRAME || r == ATSPI_ROLE_WINDOW)
                break;
            AtspiAccessible *parent = GetParent(cur);
            g_object_unref(cur);
            cur = parent;
        }
        if (cur)
            g_object_unref(cur);
    }
    g_object_unref(focus);
    if (element == nullptr) {
        error = A11yError::kUnavailable;
        return "{\"error\":\"No readable text on focused element\"}";
    }
    AtspiText *text = GetTextInterface(element);
    if (text == nullptr) {
        g_object_unref(element);
        error = A11yError::kUnavailable;
        return "{\"error\":\"No text interface\"}";
    }
    if (IsDefunctAccessible(element)) {
        g_object_unref(text);
        g_object_unref(element);
        error = A11yError::kDefunct;
        return "{\"error\":\"Text collection changed\"}";
    }
    int count = GetTextCharacterCount(text);
    int caret = GetTextCaretOffset(text);
    if (count < 0) {
        g_object_unref(text);
        g_object_unref(element);
        error = A11yError::kDefunct;
        return "{\"error\":\"Text collection changed\"}";
    }
    caret = std::clamp(caret, 0, count);
    int beforeReq = std::max(0, request.beforeChars);
    int afterReq = std::max(0, request.afterChars);
    int beforeStart = std::max(0, caret - beforeReq);
    int afterEnd = std::min(count, caret + afterReq);
    int chunk = request.textReadLimits.maxCharsPerCall > 0 ? request.textReadLimits.maxCharsPerCall
                                                           : 5000;
    std::string before, after, full;
    bool ok = ReadAccessibleText(text, beforeStart, caret, chunk, before) &&
              ReadAccessibleText(text, caret, afterEnd, chunk, after);
    bool timedOut = deadline.Expired();
    if (ok && !timedOut)
        ok = ReadAccessibleText(text, 0, std::min(count, 5000), chunk, full);
    g_object_unref(text);
    g_object_unref(element);
    if (timedOut) {
        error = A11yError::kAborted;
        return "{\"error\":\"Text collection changed\"}";
    }
    if (!ok) {
        error = A11yError::kDefunct;
        return "{\"error\":\"Text collection changed\"}";
    }
    std::string s = "{";
    s += "\"beforeContent\":" + Str(before) + ",";
    s += "\"afterContent\":" + Str(after) + ",";
    s += "\"focusedElementText\":" + Str(full) + ",";
    s += "\"beforeCharsRequested\":" + std::to_string(beforeReq) + ",";
    s += "\"afterCharsRequested\":" + std::to_string(afterReq) + ",";
    s += "\"beforeCharsActual\":" + std::to_string(caret - beforeStart) + ",";
    s += "\"afterCharsActual\":" + std::to_string(afterEnd - caret) + ",";
    s += "\"fullTextLength\":" + std::to_string(count) + ",";
    s += "\"focusedElementPosition\":" + std::to_string(caret) + ",";
    s += "\"strategy\":\"AtspiText\",";
    s += std::string("\"timeout\":") + Bool(timedOut);
    s += "}";
    return s;
}

// ---- GetVisibleText --------------------------------------------------------
// Payload {"text":"..."}: text of SHOWING nodes of the active window (DFS,
// labels/tables skipped as containers, password fields skipped), newline
// separated, capped at maxLength characters (default 20000).
size_t Utf8Length(const std::string &s)
{
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80)
            ++n;
    return n;
}

void AppendCapped(std::string &out, size_t &chars, size_t maxChars, const std::string &piece)
{
    if (piece.empty() || chars >= maxChars)
        return;
    if (!out.empty()) {
        out += "\n";
        ++chars;
    }
    for (size_t i = 0; i < piece.size() && chars < maxChars;) {
        unsigned char c = static_cast<unsigned char>(piece[i]);
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        out.append(piece, i, len);
        i += len;
        ++chars;
    }
}

std::string HandleVisibleText(const A11yRequest &request)
{
    Deadline deadline(request.timeoutMs);
    size_t maxChars = request.maxLength > 0 ? static_cast<size_t>(request.maxLength) : 20000;
    int perCall = request.textReadLimits.maxCharsPerCall > 0 ? request.textReadLimits.maxCharsPerCall
                                                             : 5000;
    AtspiAccessible *window = FindActiveWindow();
    std::string out;
    size_t chars = 0;
    if (window != nullptr && !IsDefunctAccessible(window)) {
        TreeWalkLimits limits = FocusWalkLimits(request);
        VisitChildren(
            window, limits, [&] { return deadline.Expired() || chars >= maxChars; },
            [&](AtspiAccessible *node) {
                if (IsDefunctAccessible(node) || !HasState(node, ATSPI_STATE_SHOWING))
                    return CollectionWalkStatus::kSkipChildren;
                AtspiRole role = GetRole(node);
                if (role == ATSPI_ROLE_PASSWORD_TEXT)
                    return CollectionWalkStatus::kSkipChildren;
                AtspiText *text = GetTextInterface(node);
                if (text != nullptr) {
                    int n = GetTextCharacterCount(text);
                    std::string piece;
                    if (n > 0) {
                        int want = static_cast<int>(std::min<size_t>(n, maxChars - chars));
                        if (ReadAccessibleText(text, 0, want, perCall, piece))
                            AppendCapped(out, chars, maxChars, piece);
                    }
                    g_object_unref(text);
                    // Text leaves: children are part of the same text.
                    if (role != ATSPI_ROLE_LABEL && role != ATSPI_ROLE_TABLE &&
                        GetChildCount(node) > 0 && n > 0)
                        return CollectionWalkStatus::kSkipChildren;
                } else if (GetChildCount(node) == 0) {
                    AppendCapped(out, chars, maxChars, GetName(node));
                }
                return chars >= maxChars ? CollectionWalkStatus::kStop
                                         : CollectionWalkStatus::kContinue;
            });
    }
    if (window)
        g_object_unref(window);
    (void)Utf8Length;
    return "{\"text\":" + Str(out) + "}";
}

// ---- FindDocumentUrlInActiveWindow ----------------------------------------
// Payload {"url":"<validated url or empty>","title":"<page title>"}.
// URL sources, in order: document attributes (DocURL, URI, url, URL) of the
// document containing the focus, of any document in the active window, then
// the text of a browser address bar entry. Title = window title (request
// windowTitle, else active window name) with a " - <Browser>" suffix removed.
std::string Trim(const std::string &in)
{
    size_t a = 0, b = in.size();
    while (a < b && static_cast<unsigned char>(in[a]) <= ' ')
        ++a;
    while (b > a && static_cast<unsigned char>(in[b - 1]) <= ' ')
        --b;
    return in.substr(a, b - a);
}

std::string Lower(std::string s)
{
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool HasValidTld(const std::string &host)
{
    static const char *const kTlds[] = {".com", ".org", ".net", ".edu", ".gov", ".io",
                                        ".co",  ".dev", ".app", ".cn",  ".uk",  ".de",
                                        ".jp",  ".fr",  ".au",  ".in"};
    std::string h = Lower(host);
    for (const char *t : kTlds) {
        size_t n = std::strlen(t);
        if (h.size() > n && h.compare(h.size() - n, n, t) == 0)
            return true;
    }
    return false;
}

// Returns a normalised URL or "" when the text is not a usable URL.
std::string ValidateUrl(const std::string &raw)
{
    std::string u = Trim(raw);
    if (u.empty() || u.compare(0, 6, "about:") == 0)
        return std::string();
    if (u.find_first_of(" \t\r\n") != std::string::npos)
        return std::string();
    std::string lower = Lower(u);
    if (lower.compare(0, 7, "http://") == 0 || lower.compare(0, 8, "https://") == 0 ||
        lower.compare(0, 7, "file://") == 0)
        return u;
    if (lower.find("://") != std::string::npos)
        return std::string();
    std::string host = u.substr(0, u.find_first_of("/?#"));
    size_t colon = host.find(':');
    if (colon != std::string::npos)
        host = host.substr(0, colon);
    if (!HasValidTld(host))
        return std::string();
    return "https://" + u;
}

std::string ExtractPageTitleFromWindowTitle(const std::string &title)
{
    static const char *const kSuffixes[] = {
        " - Google Chrome", " - Chrome", " - Chromium", " - Microsoft Edge", " - Edge",
        " - Mozilla Firefox", " - Firefox", " - Safari", " - Opera", " - Brave", " - Vivaldi"};
    for (const char *suf : kSuffixes) {
        size_t n = std::strlen(suf);
        if (title.size() > n && title.compare(title.size() - n, n, suf) == 0)
            return title.substr(0, title.size() - n);
    }
    return title;
}

std::string ReadDocumentUrl(AtspiAccessible *obj)
{
    AtspiDocument *doc = atspi_accessible_get_document_iface(obj);
    if (doc == nullptr)
        return std::string();
    static const char *const kAttributeKeys[] = {"DocURL", "URI", "url", "URL"};
    std::string url;
    for (const char *key : kAttributeKeys) {
        GError *err = nullptr;
        gchar *v = atspi_document_get_document_attribute_value(doc, const_cast<gchar *>(key), &err);
        if (err)
            g_error_free(err);
        if (v != nullptr) {
            url = ValidateUrl(v);
            g_free(v);
        }
        if (!url.empty())
            break;
    }
    g_object_unref(doc);
    return url;
}

bool IsDocumentRole(AtspiRole r)
{
    return r == ATSPI_ROLE_DOCUMENT_WEB || r == ATSPI_ROLE_DOCUMENT_FRAME;
}

std::string ReadTextValue(AtspiAccessible *obj)
{
    AtspiText *text = GetTextInterface(obj);
    std::string out;
    if (text != nullptr) {
        int n = GetTextCharacterCount(text);
        if (n > 0)
            ReadAccessibleText(text, 0, std::min(n, 4096), 4096, out);
        g_object_unref(text);
    }
    return out;
}

std::string HandleFindDocumentUrl(const A11yRequest &request)
{
    Deadline deadline(request.timeoutMs > 0 ? request.timeoutMs : 300);
    AtspiAccessible *window = FindActiveWindow();
    if (window != nullptr && IsDefunctAccessible(window)) {
        g_object_unref(window);
        window = nullptr;
    }
    std::string windowTitle = request.windowTitle;
    if (windowTitle.empty() && window != nullptr)
        windowTitle = GetName(window);
    std::string title = ExtractPageTitleFromWindowTitle(windowTitle);
    std::string url;

    // 1. Document around the focused element.
    TreeWalkLimits focusLimits = FocusWalkLimits(request);
    AtspiAccessible *focus = GetFocusedAccessible(FocusScanTimeout(request), focusLimits);
    if (focus != nullptr) {
        AtspiAccessible *cur = focus;
        g_object_ref(cur);
        for (int i = 0; i < 64 && cur != nullptr && url.empty() && !deadline.Expired(); ++i) {
            AtspiRole r = GetRole(cur);
            if (IsDocumentRole(r))
                url = ReadDocumentUrl(cur);
            if (r == ATSPI_ROLE_FRAME || r == ATSPI_ROLE_APPLICATION)
                break;
            AtspiAccessible *parent = GetParent(cur);
            g_object_unref(cur);
            cur = parent;
        }
        if (cur)
            g_object_unref(cur);
        g_object_unref(focus);
    }

    // 2. Any document in the active window; 3. address bar entry.
    if (url.empty() && window != nullptr && !deadline.Expired()) {
        TreeWalkLimits limits;
        limits.maxDepth = request.walkLimits.maxDepth > 0 ? request.walkLimits.maxDepth : 30;
        limits.maxBreadth = request.walkLimits.maxBreadth > 0 ? request.walkLimits.maxBreadth : 20;
        limits.maxNodes = request.walkLimits.maxNodes > 0 ? request.walkLimits.maxNodes : 600;
        std::string addressBarUrl;
        VisitChildren(
            window, limits, [&] { return deadline.Expired(); },
            [&](AtspiAccessible *node) {
                AtspiRole r = GetRole(node);
                if (IsDocumentRole(r)) {
                    url = ReadDocumentUrl(node);
                    return url.empty() ? CollectionWalkStatus::kSkipChildren
                                       : CollectionWalkStatus::kStop;
                }
                if (addressBarUrl.empty() && (r == ATSPI_ROLE_ENTRY || r == ATSPI_ROLE_TEXT)) {
                    std::string name = Lower(GetName(node));
                    if (name.find("address") != std::string::npos ||
                        name.find("location") != std::string::npos ||
                        name.find("search with") != std::string::npos)
                        addressBarUrl = ValidateUrl(ReadTextValue(node));
                }
                return CollectionWalkStatus::kContinue;
            });
        if (url.empty())
            url = addressBarUrl;
    }
    if (window)
        g_object_unref(window);
    return "{\"url\":" + Str(url) + ",\"title\":" + Str(title) + "}";
}

}  // namespace

A11yResponse DispatchA11yCommand(const A11yRequest &request)
{
    const std::string &cmd = request.command;
    if (!EnsureAccessibleReady() && cmd != kCommandEnableSessionA11yStatus &&
        cmd != kCommandIsDesktopReachable) {
        return MakeFailure(request, A11yError::kUnavailable, "atspi_not_ready");
    }

    if (cmd == kCommandEnableSessionA11yStatus) {
        bool advertised = EnableSessionAccessibility(request.reassert);
        return MakeOk(request, std::string("{\"advertised\":") + Bool(advertised) + "}");
    }
    if (cmd == kCommandIsDesktopReachable) {
        bool reachable = IsDesktopReachable();
        return MakeOk(request, std::string("{\"reachable\":") + Bool(reachable) + "}");
    }
    if (cmd == kCommandNoteForegroundProcessId) {
        NoteForegroundProcessId(request.processId);
        return MakeOk(request, "{}");
    }
    if (cmd == kCommandNoteOwnProcessForeground) {
        NoteOwnProcessForeground();
        return MakeOk(request, "{}");
    }
    if (cmd == kCommandReleaseCompositorForegroundLock) {
        ReleaseCompositorForegroundLock();
        return MakeOk(request, "{}");
    }
    if (cmd == kCommandGetActiveWindowSnapshot) {
        if (request.foregroundNote == "lock")
            NoteForegroundProcessId(request.processId);
        else if (request.foregroundNote == "own")
            NoteOwnProcessForeground();
        else if (request.foregroundNote == "release")
            ReleaseCompositorForegroundLock();
        return MakeOk(request, HandleActiveWindowSnapshot(request));
    }
    if (cmd == kCommandGetFocusSnapshot)
        return MakeOk(request, HandleFocusSnapshot(request));
    if (cmd == kCommandGetSurroundingText) {
        A11yError err = A11yError::kOk;
        std::string payload = HandleSurroundingText(request, err);
        if (err != A11yError::kOk)
            return MakeFailure(request, err, "surrounding_text_failed", payload);
        return MakeOk(request, payload);
    }
    if (cmd == kCommandGetVisibleText)
        return MakeOk(request, HandleVisibleText(request));
    if (cmd == kCommandFindDocumentUrlInActiveWindow)
        return MakeOk(request, HandleFindDocumentUrl(request));
    if (cmd == kCommandReadFocusedInputState) {
        FocusedInputState st = ReadFocusedInputState(EffectiveEditableLimits(request));
        std::string s = "{";
        s += std::string("\"valid\":") + Bool(st.valid) + ",";
        s += "\"error\":" + Str(st.error) + ",";
        s += "\"beforeText\":" + Str(st.beforeText) + ",";
        s += "\"selectedText\":" + Str(st.selectedText) + ",";
        s += "\"afterText\":" + Str(st.afterText);
        s += "}";
        return MakeOk(request, s);
    }
    if (cmd == kCommandProbeFocusedInputCapability) {
        FocusedInputCapability cap = ProbeFocusedInputCapability(EffectiveEditableLimits(request));
        const char *name = cap == FocusedInputCapability::kEditable      ? "editable"
                           : cap == FocusedInputCapability::kNotEditable ? "not-editable"
                                                                         : "unknown";
        return MakeOk(request, std::string("{\"capability\":\"") + name + "\"}");
    }
    if (cmd == kCommandRecoverAfterResume) {
        RecoverAfterResume();
        return MakeOk(request, "{}");
    }
    if (cmd == kCommandShutdown)
        return MakeOk(request, "{}");
    return MakeFailure(request, A11yError::kInvalid, "unknown_command");
}

A11yResponse HandleRequest(const A11yRequest &request)
{
    Session session;
    if (!session) {
        A11yResponse r;
        r.id = request.id;
        r.ok = false;
        r.error = A11yError::kUnavailable;
        r.failReason = "atspi_initialization_failed";
        r.payloadJson = "{}";
        return r;
    }
    DrainPendingCacheUpdates();
    try {
        return DispatchA11yCommand(request);
    } catch (...) {
        return MakeFailure(request, A11yError::kInvalid, "exception");
    }
}

void PumpApartmentEvents()
{
    PumpEvents();
}

void DrainCacheUpdates()
{
    DrainPendingCacheUpdates();
}

}  // namespace A11y
}  // namespace LinuxShared
