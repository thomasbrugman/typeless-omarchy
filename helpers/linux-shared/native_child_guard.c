/*
 * native_child_guard.c - interoperable reimplementation (see header).
 * Behaviour mirrors the x64 original (native_child_guard.c in Typeless 2.2.0).
 */
#define _GNU_SOURCE
#include "native_child_guard.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

static int g_lockFileDescriptor = -1;

int TypelessNativeChildRuntimeDirectory(const char *name, char *out, size_t out_size)
{
    if (name == NULL || name[0] == '\0' || out == NULL || out_size < 8)
        return -1;
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    int n;
    if (xdg != NULL && xdg[0] != '\0')
        n = snprintf(out, out_size, "%s/%s", xdg, name);
    else
        n = snprintf(out, out_size, "/tmp/%s-%d", name, (int)getuid());
    if (n < 0)
        return -1;
    return ((size_t)n >= out_size) ? -1 : 0;
}

int TypelessNativeChildInstallParentDeathSignal(void)
{
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0)
        return -1;
    /* The parent may have died before prctl() took effect. */
    return (getppid() <= 1) ? -1 : 0;
}

int TypelessNativeChildParentIsGone(void)
{
    return getppid() <= 1;
}

static int EnsurePrivateDirectory(const char *dir)
{
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return -1;
    chmod(dir, 0700);
    return 0;
}

int TypelessNativeChildClaimInstanceLock(const char *name, int *already_running)
{
    char dir[512];
    char lock_path[600];

    if (already_running != NULL)
        *already_running = 0;
    if (name == NULL || name[0] == '\0')
        return -1;
    if (TypelessNativeChildRuntimeDirectory(name, dir, sizeof dir) != 0)
        return -1;
    if (EnsurePrivateDirectory(dir) != 0)
        return -1;
    snprintf(lock_path, sizeof lock_path, "%s/instance.lock", dir);
    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        if (already_running != NULL)
            *already_running = 1;
        return -1;
    }
    if (g_lockFileDescriptor >= 0)
        close(g_lockFileDescriptor);
    g_lockFileDescriptor = fd;
    return 0;
}

int TypelessNativeChildBindListenSocket(const char *name, const char *socket_name,
                                        const char *pid_name, char *socket_path,
                                        size_t socket_path_size, char *pid_path,
                                        size_t pid_path_size)
{
    char dir[512];

    if (g_lockFileDescriptor < 0 || name == NULL || socket_name == NULL ||
        pid_name == NULL || socket_path == NULL || pid_path == NULL)
        return -1;
    if (TypelessNativeChildRuntimeDirectory(name, dir, sizeof dir) != 0)
        return -1;
    if (snprintf(socket_path, socket_path_size, "%s/%s", dir, socket_name) < 0)
        return -1;
    if (snprintf(pid_path, pid_path_size, "%s/%s", dir, pid_name) < 0)
        return -1;

    unlink(socket_path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    size_t len = strlen(socket_path);
    if (len > sizeof(addr.sun_path) - 1) {
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, socket_path, len + 1);

    mode_t old_mask = umask(077);
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    umask(old_mask);
    if (rc != 0) {
        close(fd);
        return -1;
    }
    chmod(socket_path, 0600);
    if (listen(fd, 8) != 0) {
        close(fd);
        unlink(socket_path);
        return -1;
    }

    FILE *f = fopen(pid_path, "we");
    if (f != NULL) {
        fprintf(f, "%d\n%d\n", (int)getpid(), (int)getppid());
        fclose(f);
        chmod(pid_path, 0600);
    }
    return fd;
}

int TypelessNativeChildClaimListenSocket(const char *name, const char *socket_name,
                                         const char *pid_name, char *socket_path,
                                         size_t socket_path_size, char *pid_path,
                                         size_t pid_path_size, int *already_running)
{
    if (TypelessNativeChildClaimInstanceLock(name, already_running) != 0)
        return -1;
    int fd = TypelessNativeChildBindListenSocket(name, socket_name, pid_name, socket_path,
                                                 socket_path_size, pid_path, pid_path_size);
    if (fd >= 0)
        return fd;
    if (g_lockFileDescriptor >= 0) {
        close(g_lockFileDescriptor);
        g_lockFileDescriptor = -1;
    }
    return -1;
}

void TypelessNativeChildReleaseListenSocket(int listen_fd, const char *socket_path,
                                            const char *pid_path)
{
    if (listen_fd >= 0)
        close(listen_fd);
    if (socket_path != NULL && socket_path[0] != '\0')
        unlink(socket_path);
    if (pid_path != NULL && pid_path[0] != '\0')
        unlink(pid_path);
    if (g_lockFileDescriptor >= 0) {
        close(g_lockFileDescriptor);
        g_lockFileDescriptor = -1;
    }
}
