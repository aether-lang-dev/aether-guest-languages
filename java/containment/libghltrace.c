// libghltrace: a recording LD_PRELOAD library for the tests, Linux only. It
// does what libaether_sandbox.so does not do for a JVM, which is let it start:
// it interposes the libc calls a sandbox would hold (open, connect, getenv,
// the exec and spawn family, fork, dlopen, syscall) and appends one line per
// call that concerns a row of stunt's catalogue to the file named by
// GHL_TRACE, then calls the real function. It refuses nothing. What it
// answers is the question the containment table cannot, with the real
// preload refusing the JVM its executable memory: which of Java's own ways
// to a file, a socket, a process or a library are libc calls an interposed
// preload would see, and which are not (java/containment/test_ways.sh).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <netinet/in.h>

static int trace_fd = -2;

static void note(const char* what, const char* arg) {
    if (trace_fd == -2) {
        const char* p = getenv("GHL_TRACE");
        trace_fd = p ? open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600) : -1;
    }
    if (trace_fd < 0) return;
    char buf[600];
    int n = snprintf(buf, sizeof buf, "%s %s\n", what, arg ? arg : "");
    if (n > 0) { ssize_t w = write(trace_fd, buf, (size_t)n); (void)w; }
}

static int interesting(const char* p) {
    return p && (strstr(p, "/etc/passwd") || strstr(p, "stunt-") || strstr(p, "/bin/sh") ||
                 strstr(p, "jspawnhelper") || strstr(p, "libm.so") || strstr(p, "libc.so") ||
                 strstr(p, "libSystem"));
}

#define REAL(ret, name, args) \
    static ret (*real_##name) args; \
    if (!real_##name) real_##name = dlsym(RTLD_NEXT, #name)

int open(const char* path, int flags, ...) {
    REAL(int, open, (const char*, int, ...));
    va_list ap; va_start(ap, flags); mode_t m = va_arg(ap, mode_t); va_end(ap);
    if (interesting(path)) note("open", path);
    return real_open(path, flags, m);
}
int open64(const char* path, int flags, ...) {
    REAL(int, open64, (const char*, int, ...));
    va_list ap; va_start(ap, flags); mode_t m = va_arg(ap, mode_t); va_end(ap);
    if (interesting(path)) note("open", path);
    return real_open64(path, flags, m);
}
int openat(int dfd, const char* path, int flags, ...) {
    REAL(int, openat, (int, const char*, int, ...));
    va_list ap; va_start(ap, flags); mode_t m = va_arg(ap, mode_t); va_end(ap);
    if (interesting(path)) note("open", path);
    return real_openat(dfd, path, flags, m);
}
int openat64(int dfd, const char* path, int flags, ...) {
    REAL(int, openat64, (int, const char*, int, ...));
    va_list ap; va_start(ap, flags); mode_t m = va_arg(ap, mode_t); va_end(ap);
    if (interesting(path)) note("open", path);
    return real_openat64(dfd, path, flags, m);
}
int connect(int fd, const struct sockaddr* sa, socklen_t len) {
    REAL(int, connect, (int, const struct sockaddr*, socklen_t));
    if (sa && sa->sa_family == AF_INET) {
        const unsigned char* a = (const unsigned char*)&((const struct sockaddr_in*)sa)->sin_addr;
        char b[64]; snprintf(b, sizeof b, "AF_INET %d.%d.%d.%d", a[0], a[1], a[2], a[3]); note("connect", b);
    } else if (sa && sa->sa_family == AF_INET6) {
        const unsigned char* a = ((const struct sockaddr_in6*)sa)->sin6_addr.s6_addr;
        char b[96]; snprintf(b, sizeof b, "AF_INET6 %s%d.%d.%d.%d", a[10] == 0xff && a[11] == 0xff ? "::ffff:" : "", a[12], a[13], a[14], a[15]);
        note("connect", b);
    }
    return real_connect(fd, sa, len);
}
char* getenv(const char* name) {
    REAL(char*, getenv, (const char*));
    if (name && strcmp(name, "HOME") == 0) note("getenv", name);
    return real_getenv(name);
}
int execve(const char* path, char* const argv[], char* const envp[]) {
    REAL(int, execve, (const char*, char* const[], char* const[]));
    if (interesting(path)) note("execve", path);
    return real_execve(path, argv, envp);
}
int posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* fa, const posix_spawnattr_t* at,
                char* const argv[], char* const envp[]) {
    REAL(int, posix_spawn, (pid_t*, const char*, const posix_spawn_file_actions_t*, const posix_spawnattr_t*, char* const[], char* const[]));
    if (interesting(path)) note("posix_spawn", path);
    return real_posix_spawn(pid, path, fa, at, argv, envp);
}
pid_t fork(void) {
    REAL(pid_t, fork, (void));
    note("fork", "");
    return real_fork();
}
void* dlopen(const char* path, int flags) {
    REAL(void*, dlopen, (const char*, int));
    if (interesting(path)) note("dlopen", path);
    return real_dlopen(path, flags);
}
long syscall(long nr, ...) {
    REAL(long, syscall, (long, ...));
    va_list ap; va_start(ap, nr);
    long a = va_arg(ap, long), b = va_arg(ap, long), c = va_arg(ap, long), d = va_arg(ap, long);
    va_end(ap);
    if (nr == SYS_openat) note("syscall", "openat");
    return real_syscall(nr, a, b, c, d);
}
