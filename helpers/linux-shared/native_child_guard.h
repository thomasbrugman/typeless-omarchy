/*
 * native_child_guard.h - interoperable reimplementation of Typeless'
 * linux-shared/native_child_guard.c (runtime dir, instance lock, listen
 * socket and parent-death helpers shared by the native child processes).
 *
 * All functions are plain C. When this file is compiled into a shared
 * library with -fvisibility=hidden nothing here is exported.
 */
#ifndef TYPELESS_NATIVE_CHILD_GUARD_H
#define TYPELESS_NATIVE_CHILD_GUARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes "$XDG_RUNTIME_DIR/<name>" (when XDG_RUNTIME_DIR is set and
 * non-empty) or "/tmp/<name>-<uid>" into out. Returns 0 on success, -1 on
 * error (bad args, out_size < 8, truncation). */
int TypelessNativeChildRuntimeDirectory(const char *name, char *out, size_t out_size);

/* prctl(PR_SET_PDEATHSIG, SIGTERM). Returns -1 if prctl failed or the
 * parent is already gone (getppid() <= 1), else 0. */
int TypelessNativeChildInstallParentDeathSignal(void);

/* 1 when getppid() <= 1 (re-parented to init/subreaper-less), else 0. */
int TypelessNativeChildParentIsGone(void);

/* mkdir(runtime dir, 0700) + chmod 0700, open "<dir>/instance.lock"
 * (O_RDWR|O_CREAT|O_CLOEXEC, 0600) and flock(LOCK_EX|LOCK_NB). The fd is
 * kept open for the life of the process. Returns 0 when the lock is held,
 * -1 otherwise; *already_running (optional) is set to 1 only when another
 * process holds the lock. */
int TypelessNativeChildClaimInstanceLock(const char *name, int *already_running);

/* Requires the instance lock. unlink()s and binds "<dir>/<socket_name>"
 * (AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, umask 077, chmod 0600, backlog 8) and
 * writes "<pid>\n<ppid>\n" to "<dir>/<pid_name>" (chmod 0600). The full
 * paths are returned in socket_path / pid_path. Returns the listen fd or -1. */
int TypelessNativeChildBindListenSocket(const char *name, const char *socket_name,
                                        const char *pid_name, char *socket_path,
                                        size_t socket_path_size, char *pid_path,
                                        size_t pid_path_size);

/* ClaimInstanceLock + BindListenSocket (drops the lock on bind failure). */
int TypelessNativeChildClaimListenSocket(const char *name, const char *socket_name,
                                         const char *pid_name, char *socket_path,
                                         size_t socket_path_size, char *pid_path,
                                         size_t pid_path_size, int *already_running);

/* close(listen_fd) (if >= 0), unlink socket_path and pid_path (if non
 * empty) and release the instance lock. */
void TypelessNativeChildReleaseListenSocket(int listen_fd, const char *socket_path,
                                            const char *pid_path);

#ifdef __cplusplus
}
#endif

#endif /* TYPELESS_NATIVE_CHILD_GUARD_H */
