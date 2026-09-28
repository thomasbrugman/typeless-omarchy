/*
 * typeless-clipboard: tiny Wayland clipboard tool for libInputHelper (arm64 port).
 *
 * Uses the focus-free ext-data-control-v1 protocol, so it works without a
 * surface and without stealing focus. Unlike wl-copy it can offer several MIME
 * types from one source (text/html + text/plain for rich text, or every type
 * of a saved clipboard when restoring it).
 *
 * Framed format used on stdin (set) and stdout (get), repeated until EOF:
 *   u32 LE mime length, mime bytes, u64 LE data length, data bytes
 *
 *   typeless-clipboard set     read frames from stdin, take the selection,
 *                              then fork into the background and serve it until
 *                              another client replaces it (like wl-copy).
 *                              The foreground process exits 0 once the
 *                              compositor has accepted the selection.
 *   typeless-clipboard get [--mime M]...
 *                              write every offered MIME type (or just the
 *                              requested ones, first available wins) with data.
 *   typeless-clipboard types   print offered MIME types, one per line.
 *   typeless-clipboard clear   clear the selection.
 *
 * Exit codes: 0 ok, 1 clipboard empty / requested type missing,
 *             2 generic failure, 3 no Wayland display or no ext-data-control.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "ext-data-control-v1-client-protocol.h"

#define EXIT_EMPTY 1
#define EXIT_FAIL 2
#define EXIT_NOPROTO 3

#define MAX_MIME_LEN 1024
#define MAX_ENTRY_BYTES (64u * 1024u * 1024u)
#define MAX_TOTAL_BYTES (96u * 1024u * 1024u)
#define MAX_ENTRIES 64

struct entry {
    char *mime;
    unsigned char *data;
    size_t len;
};

static struct wl_display *g_display;
static struct wl_seat *g_seat;
static struct ext_data_control_manager_v1 *g_manager;

static struct entry g_entries[MAX_ENTRIES];
static size_t g_entry_count;

static int g_cancelled;

/* ---- offers (get mode) ---- */
struct offer {
    struct ext_data_control_offer_v1 *obj;
    char *mimes[MAX_ENTRIES];
    size_t mime_count;
};
static struct offer *g_current_offer;   /* most recently announced offer */
static struct offer *g_selection_offer; /* offer attached to the selection */
static int g_selection_seen;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- registry ---- */
static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, wl_seat_interface.name) == 0 && !g_seat) {
        g_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
        g_manager = wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int connect_wayland(void)
{
    const char *wd = getenv("WAYLAND_DISPLAY");
    if (!wd || !*wd) {
        return EXIT_NOPROTO;
    }
    g_display = wl_display_connect(NULL);
    if (!g_display) {
        return EXIT_NOPROTO;
    }
    struct wl_registry *registry = wl_display_get_registry(g_display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(g_display) < 0) {
        return EXIT_FAIL;
    }
    if (!g_seat || !g_manager) {
        return EXIT_NOPROTO;
    }
    return 0;
}

/* ---- framing ---- */
static int read_full(int fd, void *buf, size_t len)
{
    unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return 0;
        }
        p += n;
        len -= (size_t)n;
    }
    return 1;
}

static int write_full(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_frames_from_stdin(void)
{
    size_t total = 0;
    for (;;) {
        uint32_t mime_len;
        int r = read_full(STDIN_FILENO, &mime_len, sizeof mime_len);
        if (r == 0) {
            break;
        }
        if (r < 0 || mime_len == 0 || mime_len > MAX_MIME_LEN || g_entry_count >= MAX_ENTRIES) {
            return -1;
        }
        char *mime = calloc(1, mime_len + 1);
        if (!mime || read_full(STDIN_FILENO, mime, mime_len) != 1) {
            return -1;
        }
        uint64_t data_len;
        if (read_full(STDIN_FILENO, &data_len, sizeof data_len) != 1 || data_len > MAX_ENTRY_BYTES) {
            return -1;
        }
        total += data_len;
        if (total > MAX_TOTAL_BYTES) {
            return -1;
        }
        unsigned char *data = malloc(data_len ? data_len : 1);
        if (!data || (data_len && read_full(STDIN_FILENO, data, data_len) != 1)) {
            return -1;
        }
        g_entries[g_entry_count].mime = mime;
        g_entries[g_entry_count].data = data;
        g_entries[g_entry_count].len = data_len;
        g_entry_count++;
    }
    return 0;
}

static int write_frame(int fd, const char *mime, const unsigned char *data, size_t len)
{
    uint32_t mime_len = (uint32_t)strlen(mime);
    uint64_t data_len = len;
    if (write_full(fd, &mime_len, sizeof mime_len) < 0 || write_full(fd, mime, mime_len) < 0 ||
        write_full(fd, &data_len, sizeof data_len) < 0 || (len && write_full(fd, data, len) < 0)) {
        return -1;
    }
    return 0;
}

/* ---- source (set mode) ---- */
static void write_to_client(int fd, const struct entry *e)
{
    /* Never block forever on a client that stops reading. */
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    size_t off = 0;
    int64_t deadline = now_ms() + 5000;
    while (off < e->len) {
        ssize_t n = write(fd, e->data + off, e->len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int64_t left = deadline - now_ms();
            if (left <= 0) {
                break;
            }
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            if (poll(&pfd, 1, (int)left) <= 0) {
                break;
            }
            continue;
        }
        break;
    }
}

static void source_send(void *data, struct ext_data_control_source_v1 *source, const char *mime_type,
                        int32_t fd)
{
    (void)data;
    (void)source;
    for (size_t i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].mime, mime_type) == 0) {
            write_to_client(fd, &g_entries[i]);
            break;
        }
    }
    close(fd);
}

static void source_cancelled(void *data, struct ext_data_control_source_v1 *source)
{
    (void)data;
    (void)source;
    g_cancelled = 1;
}

static const struct ext_data_control_source_v1_listener source_listener = {
    .send = source_send,
    .cancelled = source_cancelled,
};

/* ---- device + offers ---- */
static void offer_offer(void *data, struct ext_data_control_offer_v1 *obj, const char *mime_type)
{
    (void)obj;
    struct offer *o = data;
    if (o->mime_count < MAX_ENTRIES) {
        o->mimes[o->mime_count++] = strdup(mime_type);
    }
}

static const struct ext_data_control_offer_v1_listener offer_listener = {
    .offer = offer_offer,
};

static void device_data_offer(void *data, struct ext_data_control_device_v1 *device,
                              struct ext_data_control_offer_v1 *id)
{
    (void)data;
    (void)device;
    struct offer *o = calloc(1, sizeof *o);
    if (!o) {
        return;
    }
    o->obj = id;
    ext_data_control_offer_v1_add_listener(id, &offer_listener, o);
    g_current_offer = o;
}

static void device_selection(void *data, struct ext_data_control_device_v1 *device,
                             struct ext_data_control_offer_v1 *id)
{
    (void)data;
    (void)device;
    g_selection_seen = 1;
    if (!id) {
        g_selection_offer = NULL;
        return;
    }
    if (g_current_offer && g_current_offer->obj == id) {
        g_selection_offer = g_current_offer;
    } else {
        g_selection_offer = wl_proxy_get_user_data((struct wl_proxy *)id);
    }
}

static void device_finished(void *data, struct ext_data_control_device_v1 *device)
{
    (void)data;
    (void)device;
    g_cancelled = 1;
}

static void device_primary_selection(void *data, struct ext_data_control_device_v1 *device,
                                     struct ext_data_control_offer_v1 *id)
{
    (void)data;
    (void)device;
    (void)id;
}

static const struct ext_data_control_device_v1_listener device_listener = {
    .data_offer = device_data_offer,
    .selection = device_selection,
    .finished = device_finished,
    .primary_selection = device_primary_selection,
};

/* Pseudo-targets some X11 bridges advertise; re-offering them is meaningless. */
static int is_pseudo_target(const char *mime)
{
    static const char *const skip[] = {"TARGETS", "MULTIPLE", "TIMESTAMP", "SAVE_TARGETS", "DELETE",
                                       "INCR", NULL};
    for (int i = 0; skip[i]; i++) {
        if (strcmp(mime, skip[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int receive_mime(struct offer *o, const char *mime, unsigned char **out, size_t *out_len,
                        int64_t deadline)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) {
        return -1;
    }
    ext_data_control_offer_v1_receive(o->obj, mime, fds[1]);
    wl_display_flush(g_display);
    close(fds[1]);

    size_t cap = 4096, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) {
        close(fds[0]);
        return -1;
    }
    int ok = -1;
    for (;;) {
        int64_t left = deadline - now_ms();
        if (left <= 0) {
            break;
        }
        struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
        int pr = poll(&pfd, 1, (int)left);
        if (pr < 0 && errno == EINTR) {
            continue;
        }
        if (pr <= 0) {
            break;
        }
        if (len == cap) {
            if (cap >= MAX_ENTRY_BYTES) {
                break;
            }
            cap *= 2;
            unsigned char *nb = realloc(buf, cap);
            if (!nb) {
                break;
            }
            buf = nb;
        }
        ssize_t n = read(fds[0], buf + len, cap - len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            break;
        }
        if (n == 0) {
            ok = 0;
            break;
        }
        len += (size_t)n;
    }
    close(fds[0]);
    if (ok != 0) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = len;
    return 0;
}

static int bind_device(struct ext_data_control_device_v1 **device_out)
{
    struct ext_data_control_device_v1 *device =
        ext_data_control_manager_v1_get_data_device(g_manager, g_seat);
    ext_data_control_device_v1_add_listener(device, &device_listener, NULL);
    /* The initial selection event is sent right after device creation. */
    if (wl_display_roundtrip(g_display) < 0) {
        return EXIT_FAIL;
    }
    if (!g_selection_seen && wl_display_roundtrip(g_display) < 0) {
        return EXIT_FAIL;
    }
    *device_out = device;
    return 0;
}

static int cmd_get(int argc, char **argv, int types_only)
{
    const char *wanted[16];
    int wanted_count = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--mime") == 0 && i + 1 < argc && wanted_count < 16) {
            wanted[wanted_count++] = argv[++i];
        }
    }
    int rc = connect_wayland();
    if (rc) {
        return rc;
    }
    struct ext_data_control_device_v1 *device;
    rc = bind_device(&device);
    if (rc) {
        return rc;
    }
    struct offer *o = g_selection_offer;
    if (!o) {
        return EXIT_EMPTY;
    }
    if (types_only) {
        for (size_t i = 0; i < o->mime_count; i++) {
            printf("%s\n", o->mimes[i]);
        }
        fflush(stdout);
        return 0;
    }
    int64_t deadline = now_ms() + (wanted_count > 0 ? 1500 : 1000);
    size_t total = 0;
    int wrote = 0;
    if (wanted_count > 0) {
        for (int w = 0; w < wanted_count && !wrote; w++) {
            for (size_t i = 0; i < o->mime_count; i++) {
                if (strcmp(o->mimes[i], wanted[w]) != 0) {
                    continue;
                }
                unsigned char *buf;
                size_t len;
                int64_t per = now_ms() + 1000;
                if (receive_mime(o, o->mimes[i], &buf, &len, per < deadline ? per : deadline) == 0) {
                    write_frame(STDOUT_FILENO, o->mimes[i], buf, len);
                    free(buf);
                    wrote = 1;
                }
                break;
            }
        }
        return wrote ? 0 : EXIT_EMPTY;
    }
    /* Snapshot for a later restore. The caller is waiting (the app's JS thread),
     * and sources such as browsers re-encode images per request, so: text
     * types first, then other types, but only one image type (image/png if
     * offered), all within the overall deadline. */
    const char *image_pick = NULL;
    for (size_t i = 0; i < o->mime_count; i++) {
        if (strncmp(o->mimes[i], "image/", 6) == 0 &&
            (!image_pick || strcmp(o->mimes[i], "image/png") == 0)) {
            image_pick = o->mimes[i];
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < o->mime_count; i++) {
            const char *mime = o->mimes[i];
            int is_text = strncmp(mime, "text/", 5) == 0 || strcmp(mime, "UTF8_STRING") == 0 ||
                          strcmp(mime, "STRING") == 0 || strcmp(mime, "TEXT") == 0;
            if (is_pseudo_target(mime) || (pass == 0) != is_text) {
                continue;
            }
            if (strncmp(mime, "image/", 6) == 0 && strcmp(mime, image_pick) != 0) {
                continue;
            }
            int duplicate = 0; /* some sources announce a type twice */
            for (size_t j = 0; j < i; j++) {
                if (strcmp(o->mimes[j], mime) == 0) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            if (now_ms() >= deadline) {
                return 0;
            }
            unsigned char *buf;
            size_t len;
            int64_t per = now_ms() + 500;
            if (receive_mime(o, mime, &buf, &len, per < deadline ? per : deadline) != 0) {
                continue;
            }
            if (total + len > MAX_TOTAL_BYTES) {
                free(buf);
                continue;
            }
            total += len;
            write_frame(STDOUT_FILENO, mime, buf, len);
            free(buf);
        }
    }
    return 0;
}

static int cmd_clear(void)
{
    int rc = connect_wayland();
    if (rc) {
        return rc;
    }
    struct ext_data_control_device_v1 *device =
        ext_data_control_manager_v1_get_data_device(g_manager, g_seat);
    ext_data_control_device_v1_add_listener(device, &device_listener, NULL);
    ext_data_control_device_v1_set_selection(device, NULL);
    if (wl_display_roundtrip(g_display) < 0) {
        return EXIT_FAIL;
    }
    return 0;
}

static int cmd_set(int foreground)
{
    if (read_frames_from_stdin() < 0 || g_entry_count == 0) {
        return EXIT_FAIL;
    }
    int rc = connect_wayland();
    if (rc) {
        return rc;
    }
    struct ext_data_control_device_v1 *device =
        ext_data_control_manager_v1_get_data_device(g_manager, g_seat);
    ext_data_control_device_v1_add_listener(device, &device_listener, NULL);
    struct ext_data_control_source_v1 *source = ext_data_control_manager_v1_create_data_source(g_manager);
    ext_data_control_source_v1_add_listener(source, &source_listener, NULL);
    for (size_t i = 0; i < g_entry_count; i++) {
        ext_data_control_source_v1_offer(source, g_entries[i].mime);
    }
    ext_data_control_device_v1_set_selection(device, source);
    if (wl_display_roundtrip(g_display) < 0 || g_cancelled) {
        return EXIT_FAIL;
    }

    if (!foreground) {
        /* Report success to the caller, keep serving in a detached grandchild. */
        fflush(stdout);
        pid_t pid = fork();
        if (pid < 0) {
            return EXIT_FAIL;
        }
        if (pid > 0) {
            _exit(0);
        }
        setsid();
        int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        if (chdir("/") != 0) {
            /* ignore */
        }
    }
    signal(SIGPIPE, SIG_IGN);
    while (!g_cancelled) {
        if (wl_display_dispatch(g_display) < 0) {
            break;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    /* Make sure 0-2 are open so the Wayland socket never lands on a stdio fd
     * (daemonizing redirects those to /dev/null). */
    for (int fd = 0; fd <= 2; fd++) {
        if (fcntl(fd, F_GETFD) < 0) {
            int nfd = open("/dev/null", O_RDWR);
            if (nfd >= 0 && nfd != fd) {
                dup2(nfd, fd);
                close(nfd);
            }
        }
    }
    if (argc < 2) {
        fprintf(stderr, "usage: %s set [--foreground] | get [--mime M]... | types | clear\n", argv[0]);
        return EXIT_FAIL;
    }
    if (strcmp(argv[1], "set") == 0) {
        int fg = argc > 2 && strcmp(argv[2], "--foreground") == 0;
        return cmd_set(fg);
    }
    if (strcmp(argv[1], "get") == 0) {
        return cmd_get(argc - 2, argv + 2, 0);
    }
    if (strcmp(argv[1], "types") == 0) {
        return cmd_get(0, NULL, 1);
    }
    if (strcmp(argv[1], "clear") == 0) {
        return cmd_clear();
    }
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return EXIT_FAIL;
}
