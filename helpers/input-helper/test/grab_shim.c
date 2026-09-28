// Test-only LD_PRELOAD shim: when libInputHelper creates its uinput keyboard,
// grab the resulting evdev node exclusively (EVIOCGRAB) before ioctl() returns,
// so no injected key can ever reach the real compositor / focused window.
// If the grab fails the process aborts before a single key is written.
// Every key event seen on the grabbed node is appended to $GRAB_SHIM_LOG as
// "<code> <value>" lines; "SYN" lines are omitted.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef int (*ioctl_fn)(int, unsigned long, ...);

static int g_grab_fd = -1;

static void *reader(void *arg)
{
    (void)arg;
    const char *log = getenv("GRAB_SHIM_LOG");
    FILE *f = log ? fopen(log, "a") : stderr;
    struct input_event ev;
    for (;;) {
        ssize_t n = read(g_grab_fd, &ev, sizeof ev);
        if (n == (ssize_t)sizeof ev) {
            if (ev.type == EV_KEY) {
                fprintf(f, "%d %d\n", ev.code, ev.value);
                fflush(f);
            }
        } else if (n < 0 && errno != EINTR) {
            break;
        }
    }
    return NULL;
}

static void grab_or_die(int uinput_fd, ioctl_fn real)
{
    char sysname[64] = {0};
    if (real(uinput_fd, UI_GET_SYSNAME(sizeof sysname), sysname) < 0) {
        fprintf(stderr, "[grab_shim] UI_GET_SYSNAME failed: %s -> abort\n", strerror(errno));
        abort();
    }
    char dirpath[256];
    snprintf(dirpath, sizeof dirpath, "/sys/devices/virtual/input/%s", sysname);
    char node[300] = {0};
    for (int attempt = 0; attempt < 200 && !node[0]; attempt++) {  // wait for udev, <= 1 s
        DIR *d = opendir(dirpath);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                if (strncmp(de->d_name, "event", 5) == 0) {
                    snprintf(node, sizeof node, "/dev/input/%s", de->d_name);
                    break;
                }
            }
            closedir(d);
        }
        if (!node[0]) usleep(5000);
    }
    if (!node[0]) {
        fprintf(stderr, "[grab_shim] no event node for %s -> abort\n", sysname);
        abort();
    }
    int fd = -1;
    for (int attempt = 0; attempt < 200 && fd < 0; attempt++) {  // node permissions settle via udev
        fd = open(node, O_RDONLY | O_CLOEXEC);
        if (fd < 0) usleep(5000);
    }
    if (fd < 0 || real(fd, EVIOCGRAB, (void *)1) < 0) {
        fprintf(stderr, "[grab_shim] cannot grab %s: %s -> abort\n", node, strerror(errno));
        abort();
    }
    g_grab_fd = fd;
    fprintf(stderr, "[grab_shim] exclusively grabbed %s (%s)\n", node, sysname);
    pthread_t t;
    pthread_create(&t, NULL, reader, NULL);
    pthread_detach(t);
}

int ioctl(int fd, unsigned long request, ...)
{
    static ioctl_fn real;
    if (!real) real = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    int rc = real(fd, request, arg);
    if (request == UI_DEV_CREATE && rc == 0) grab_or_die(fd, real);
    return rc;
}
