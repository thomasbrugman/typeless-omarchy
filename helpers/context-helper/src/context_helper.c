// libContextHelper.so - interoperable Linux arm64 reimplementation of the Typeless
// context helper C ABI (loaded in the Electron main process through koffi).
//
// Exported ABI (matches the x64 build):
//   const char *context_say_hello(void)                       static string
//   void  initialize_context_helper(void)
//   void  cleanup_context_helper(void)
//   void  recover_context_helper_after_resume(void)
//   void  free_string(const char *)                           frees our malloc()ed strings
//   char *getFocusedAppInfo(void)                             malloc()ed JSON
//   void  getFocusedAppInfoAsync(void (*cb)(const char *))
//   char *getFocusedElementInfo(void)                         malloc()ed JSON
//   void  getFocusedElementInfoAsync(void (*cb)(const char *))
//   char *getFocusedElementRelatedContent(int before, int after, int timeoutMs)
//   void  getFocusedElementRelatedContentAsync(int, int, int, void (*cb)(const char *))
//   char *getFocusedVisibleText(int maxLength, int timeoutMs)  plain text
//   void  getFocusedVisibleTextAsync(int, int, void (*cb)(const char *))
//   int   setFocusedWindowEnhancedUserInterface(void)          1 ok, -1 no a11y bus, -2 failed
//
// Async variants return immediately; the work runs on our own threads and the
// callback is invoked exactly once from that thread (koffi relays callbacks
// made on foreign threads to the JS thread and blocks until they ran, so the
// string passed stays valid for the duration of the call and is freed after).
// If a newer request of the same kind is queued before an older one started,
// the older one is answered with the x64 build's "evicted" payload. Nothing is
// delivered once cleanup_context_helper() has begun (same as the original).
#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "a11y.h"
#include "util.h"
#include "window.h"

#define EXPORT __attribute__((visibility("default")))

typedef void (*str_cb)(const char *);

static volatile int g_shutting_down;

enum { K_APP, K_ELEM, K_RELATED, K_VISIBLE, K_WARMUP, K_ENABLE };

typedef struct job {
    int kind;
    int a, b, c;
    str_cb cb;
    struct job *next;
} job;

typedef struct worker {
    const char *name;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    job *head, *tail;
    bool stop, exited;
    pthread_t th;
} worker;

static pthread_mutex_t g_workers_mu = PTHREAD_MUTEX_INITIALIZER;
static worker *g_wapp, *g_wa11y, *g_wvis; // app info | element+related | visible text
static bool g_warmed;

// ---------------------------------------------------------------------------

static char *run_job(const job *j) {
    switch (j->kind) {
    case K_APP: return build_app_info_json();
    case K_ELEM: return a11y_focus_snapshot_json(300);
    case K_RELATED: return a11y_related_content_json(j->a, j->b, j->c);
    case K_VISIBLE: return a11y_visible_text(j->a, j->b);
    case K_WARMUP: a11y_desktop_reachable(); return NULL;
    case K_ENABLE:
        if (a11y_desktop_reachable()) a11y_enable_session(true);
        return NULL;
    }
    return NULL;
}

static char *evicted_payload(int kind) {
    switch (kind) {
    case K_APP: return xstrdup("{}");
    case K_ELEM: return xstrdup("{}");
    case K_RELATED: return xstrdup("{\"error\":\"evicted\"}");
    case K_VISIBLE: return xstrdup("");
    }
    return NULL;
}

static void deliver(str_cb cb, char *s) {
    if (cb && s && !g_shutting_down) cb(s);
    free(s);
}

static void *worker_main(void *arg) {
    worker *w = arg;
    pthread_setname_np(pthread_self(), w->name);
    pthread_mutex_lock(&w->mu);
    for (;;) {
        while (!w->head && !w->stop) pthread_cond_wait(&w->cv, &w->mu);
        if (w->stop) break;
        job *j = w->head;
        w->head = j->next;
        if (!w->head) w->tail = NULL;
        bool evict = false;
        for (job *k = w->head; k; k = k->next)
            if (k->kind == j->kind) evict = true;
        pthread_mutex_unlock(&w->mu);
        if (evict) deliver(j->cb, evicted_payload(j->kind));
        else if (!g_shutting_down) {
            char *s = run_job(j);
            if (j->cb) {
                if (!s) s = evicted_payload(j->kind);
                deliver(j->cb, s);
            } else {
                free(s);
            }
        }
        free(j);
        pthread_mutex_lock(&w->mu);
    }
    // Drop anything still queued without answering (shutdown).
    job *j = w->head;
    w->head = w->tail = NULL;
    w->exited = true;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    while (j) {
        job *n = j->next;
        free(j);
        j = n;
    }
    return NULL;
}

static worker *worker_get(worker **slot, const char *name) {
    pthread_mutex_lock(&g_workers_mu);
    worker *w = *slot;
    if (!w) {
        w = calloc(1, sizeof *w);
        if (w) {
            w->name = name;
            pthread_mutex_init(&w->mu, NULL);
            pthread_cond_init(&w->cv, NULL);
            pthread_attr_t a;
            pthread_attr_init(&a);
            pthread_attr_setstacksize(&a, 1024 * 1024);
            if (pthread_create(&w->th, &a, worker_main, w) != 0) {
                free(w);
                w = NULL;
            }
            pthread_attr_destroy(&a);
            *slot = w;
        }
    }
    pthread_mutex_unlock(&g_workers_mu);
    return w;
}

static void post(worker **slot, const char *name, int kind, int a, int b, int c, str_cb cb) {
    if (g_shutting_down) return; // original: silently ignored while shutting down
    worker *w = worker_get(slot, name);
    job *j = calloc(1, sizeof *j);
    if (!w || !j) {
        free(j);
        // Could not queue: answer right away so the JS side never leaks its
        // registered callback.
        deliver(cb, evicted_payload(kind));
        return;
    }
    j->kind = kind;
    j->a = a;
    j->b = b;
    j->c = c;
    j->cb = cb;
    pthread_mutex_lock(&w->mu);
    if (w->tail) w->tail->next = j;
    else w->head = j;
    w->tail = j;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

static void warmup_once(void) {
    // Like the x64 build (helper spawned on the first app-info query), connect
    // to the accessibility bus early so toolkits can build their trees before
    // the first dictation needs them.
    pthread_mutex_lock(&g_workers_mu);
    bool need = !g_warmed;
    g_warmed = true;
    pthread_mutex_unlock(&g_workers_mu);
    if (need) post(&g_wa11y, "ctxh-a11y", K_WARMUP, 0, 0, 0, NULL);
}

static void worker_stop(worker **slot) {
    pthread_mutex_lock(&g_workers_mu);
    worker *w = *slot;
    *slot = NULL;
    pthread_mutex_unlock(&g_workers_mu);
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    w->stop = true;
    pthread_cond_broadcast(&w->cv);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;
    while (!w->exited)
        if (pthread_cond_timedwait(&w->cv, &w->mu, &ts) != 0) break;
    bool exited = w->exited;
    pthread_mutex_unlock(&w->mu);
    if (exited) {
        pthread_join(w->th, NULL);
        pthread_cond_destroy(&w->cv);
        pthread_mutex_destroy(&w->mu);
        free(w);
    } else {
        // Stuck inside a JS callback or D-Bus call; let it finish on its own.
        pthread_detach(w->th);
    }
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

EXPORT const char *context_say_hello(void) { return "Hello from Context Helper Linux Implementation!"; }

EXPORT void initialize_context_helper(void) {
    g_shutting_down = 0;
    warmup_once();
}

EXPORT void cleanup_context_helper(void) {
    g_shutting_down = 1;
    worker_stop(&g_wapp);
    worker_stop(&g_wa11y);
    worker_stop(&g_wvis);
    a11y_shutdown();
    pthread_mutex_lock(&g_workers_mu);
    g_warmed = false;
    pthread_mutex_unlock(&g_workers_mu);
}

EXPORT void recover_context_helper_after_resume(void) { a11y_recover_after_resume(); }

EXPORT void free_string(const char *s) { free((void *)s); }

EXPORT char *getFocusedAppInfo(void) {
    char *s = build_app_info_json();
    return s ? s : xstrdup("{}");
}

EXPORT void getFocusedAppInfoAsync(str_cb cb) {
    if (!cb || g_shutting_down) return;
    warmup_once();
    post(&g_wapp, "ctxh-app", K_APP, 0, 0, 0, cb);
}

EXPORT char *getFocusedElementInfo(void) { return a11y_focus_snapshot_json(300); }

EXPORT void getFocusedElementInfoAsync(str_cb cb) {
    if (!cb || g_shutting_down) return;
    post(&g_wa11y, "ctxh-a11y", K_ELEM, 0, 0, 0, cb);
}

EXPORT char *getFocusedElementRelatedContent(int before, int after, int timeout_ms) {
    return a11y_related_content_json(before, after, timeout_ms);
}

EXPORT void getFocusedElementRelatedContentAsync(int before, int after, int timeout_ms, str_cb cb) {
    if (!cb || g_shutting_down) return;
    post(&g_wa11y, "ctxh-a11y", K_RELATED, before, after, timeout_ms, cb);
}

EXPORT char *getFocusedVisibleText(int max_len, int timeout_ms) { return a11y_visible_text(max_len, timeout_ms); }

EXPORT void getFocusedVisibleTextAsync(int max_len, int timeout_ms, str_cb cb) {
    if (!cb || g_shutting_down) return;
    post(&g_wvis, "ctxh-vis", K_VISIBLE, max_len, timeout_ms, 0, cb);
}

EXPORT int setFocusedWindowEnhancedUserInterface(void) {
    // Called synchronously on the JS thread right before every full-context
    // fetch. Never block it: (re)assert org.a11y.Status on the a11y worker,
    // which runs FIFO, so it still happens before the element queries that
    // follow. The return value reflects the last known state (the app ignores it).
    if (g_shutting_down) return -1;
    warmup_once();
    post(&g_wa11y, "ctxh-a11y", K_ENABLE, 0, 0, 0, NULL);
    return a11y_enable_status_code();
}
