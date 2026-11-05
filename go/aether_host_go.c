// aether_host_go.c — Go Language Host Module
//
// Go ships its own runtime + scheduler that can't coexist with
// another runtime in the same process. Rather than fighting that,
// this host runs Go code as a separate subprocess with the Aether
// LD_PRELOAD sandbox library injected. Same pattern as
// contrib/host/aether/, adapted for the Go toolchain.
//
// Two modes: run a pre-built binary, or shell out to `go run
// script.go`. In the second case the `go` toolchain itself runs
// under LD_PRELOAD, so its own libc calls (reading $GOROOT,
// invoking linker, etc.) are sandbox-checked. That's either useful
// or annoying depending on the use case — for defense-in-depth,
// pre-build the binary and use run_sandboxed to narrow the grant
// surface to just what the Go program itself does.
//
// Linux-only for real enforcement. LD_PRELOAD doesn't work under
// macOS System Integrity Protection; on macOS the binary runs
// without preload and the grants are advisory only (in-process
// checker has nothing to intercept).

#include "aether_host_go.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) || defined(__APPLE__)

#include "aether_sandbox.h"
#include "aether_shared_map.h"
/* The process's spawn discipline (#3040, #3046): its close-on-exec pipe and its fork. */
#include "utils/aether_spawn.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

extern int list_size(void*);
extern void* list_get_raw(void*, int);
/* A grant list std.sandbox built holds plain C strings; one a program
 * built can hold Aether strings (a helper that keeps its `string`
 * parameter stores its own copy). This reads the bytes of either. */
extern const char* aether_string_data(const void*);

// -----------------------------------------------------------------
// Locate the `go` toolchain. Honour $GO_BIN if set, otherwise fall
// back to "go" (relying on $PATH at exec time).
// -----------------------------------------------------------------
static const char* get_go_path(void) {
    const char* env = getenv("GO_BIN");
    if (env && env[0]) return env;
    return "go";
}

// Where libaether_sandbox.so is: the runtime's lookup, shared with
// spawn_sandboxed (#3177).
#include "sandbox/aether_sandbox_preload_path.h"

// -----------------------------------------------------------------
// Serialize grant list to shared memory for the child's sandbox
// preload library to pick up. Format matches what
// libaether_sandbox_preload.c expects: "category:pattern\n" lines.
// -----------------------------------------------------------------
static char* serialize_grants_to_shm(void* perms) {
    if (!perms) return NULL;
    int n = list_size(perms);
    /* An empty list is a grant list: nothing is granted. aether's copy
     * returned NULL for it, so a run with no grants failed to start
     * instead of running with everything refused. */
    if (n < 0 || n % 2 != 0) return NULL;

    char buf[8192];
    int pos = 0;
    buf[0] = '\0';
    for (int i = 0; i < n && pos < 8000; i += 2) {
        const char* cat = aether_string_data(list_get_raw(perms, i));
        const char* pat = aether_string_data(list_get_raw(perms, i + 1));
        if (!cat || !pat) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s:%s\n", cat, pat);
    }

    char* shm_name = malloc(64);
    if (!shm_name) return NULL;
    snprintf(shm_name, 64, "/aether_host_go_%d", getpid());

    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { free(shm_name); return NULL; }

    if (ftruncate(fd, pos + 1) != 0) {
        close(fd);
        shm_unlink(shm_name);
        free(shm_name);
        return NULL;
    }
    void* mem = mmap(NULL, pos + 1, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        shm_unlink(shm_name);
        free(shm_name);
        return NULL;
    }

    memcpy(mem, buf, pos + 1);
    munmap(mem, pos + 1);
    close(fd);
    return shm_name;
}

// -----------------------------------------------------------------
// Core: fork + exec with sandbox preload.
// argv is NULL-terminated. program is argv[0], looked up on PATH in
// the parent so "go" works without an absolute path.
//
// Everything the child execs with is made here (#3200): the program's
// path, the /bin/sh argv for a file with no #! line (execvp ran those
// under /bin/sh), and the env with the preload, the grant source and
// the map put in. The child used to setenv them and call execvp between
// fork and exec, which a child of a threaded parent may not do: setenv
// mallocs and takes the environment's lock, and a lock another thread
// held at the fork stays held in the child. It now calls only execve.
// -----------------------------------------------------------------
static int spawn_go_sandboxed(void* perms,
                              const char* program,
                              char* const argv[],
                              uint64_t map_token) {
    if (!program || !argv) return -1;

    // Preload library.
    char preload_path[1024];
    if (perms) {
        char tried[2048];
        if (!aether_sandbox_find_preload(preload_path, sizeof(preload_path), tried, sizeof(tried))) {
            aether_sandbox_preload_missing("[go-host]", tried);
            return -1;
        }
    }

    // Grants → shm.
    char* grants_shm = NULL;
    if (perms) {
        grants_shm = serialize_grants_to_shm(perms);
        if (!grants_shm) {
            fprintf(stderr,
                    "[go-host] cannot serialize grants to shm\n");
            return -1;
        }
    }

    // Shared map → shm.
    char* map_shm = NULL;
    if (map_token != 0) {
        extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
        aether_shared_map_freeze_inputs_by_token(map_token);
        map_shm = aether_shared_map_to_shm_by_token(map_token);
        if (!map_shm) {
            fprintf(stderr,
                    "[go-host] cannot serialize map to shm\n");
            if (grants_shm) {
                shm_unlink(grants_shm); free(grants_shm);
            }
            return -1;
        }
    }

    char preload_var[sizeof preload_path + 16];
    char grants_var[512];
    char map_var[512];
    const char* set[3];
    int nset = 0;
    if (perms) {
        snprintf(preload_var, sizeof preload_var, "LD_PRELOAD=%s", preload_path);
        snprintf(grants_var, sizeof grants_var, "AETHER_SANDBOX_SHM=%s", grants_shm);
        set[nset++] = preload_var;
        set[nset++] = grants_var;
    }
    if (map_shm) {
        snprintf(map_var, sizeof map_var, "AETHER_MAP_SHM=%s", map_shm);
        set[nset++] = map_var;
    }
    size_t argc = 0;
    while (argv[argc]) argc++;
    char* path = aether_spawn_resolve(program);
    char** env = aether_spawn_env(aether_spawn_environ(), set, nset);
    char** sh_argv = (char**)malloc((argc + 2) * sizeof(char*));
    /* A program that cannot start is an error, -1, as std.os reports one
     * (#3170), never an exit of 127, which a program that ran can give: one
     * on no PATH entry here, and one whose exec fails through this
     * close-on-exec pipe, where the child writes its errno and an exec that
     * succeeds closes it unwritten. */
    int errp[2] = { -1, -1 };
    if (!path || !env || !sh_argv || aether_pipe_cloexec(errp) != 0) {
        free(path); free(env); free(sh_argv);
        if (grants_shm) { shm_unlink(grants_shm); free(grants_shm); }
        if (map_shm) {
            aether_shared_map_unlink_shm(map_shm); free(map_shm);
        }
        return -1;
    }
    sh_argv[0] = (char*)"/bin/sh";
    sh_argv[1] = path;
    for (size_t i = 1; i <= argc; i++) sh_argv[i + 1] = argv[i];

    pid_t pid = aether_fork();
    if (pid < 0) {
        free(path); free(env); free(sh_argv);
        close(errp[0]); close(errp[1]);
        if (grants_shm) { shm_unlink(grants_shm); free(grants_shm); }
        if (map_shm) {
            aether_shared_map_unlink_shm(map_shm); free(map_shm);
        }
        return -1;
    }

    if (pid == 0) {
        // Child: only async-signal-safe calls from here (#3200).
        close(errp[0]);
        execve(path, argv, env);
        if (errno == ENOEXEC) {
            execve("/bin/sh", sh_argv, env);
            errno = ENOEXEC;
        }
        int e = errno;
        ssize_t w;
        do { w = write(errp[1], &e, sizeof e); } while (w < 0 && errno == EINTR);
        _exit(127);
    }
    free(path); free(env); free(sh_argv);
    close(errp[1]);
    int exec_errno = 0;
    ssize_t got;
    do { got = read(errp[0], &exec_errno, sizeof exec_errno); } while (got < 0 && errno == EINTR);
    close(errp[0]);
    int not_started = got == (ssize_t)sizeof exec_errno;

    int status = 0;
    waitpid(pid, &status, 0);

    if (map_shm && map_token != 0) {
        aether_shared_map_read_outputs_from_shm_by_token(map_token, map_shm);
    }
    if (grants_shm) { shm_unlink(grants_shm); free(grants_shm); }
    if (map_shm) {
        aether_shared_map_unlink_shm(map_shm); free(map_shm);
    }

    if (not_started) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

// -----------------------------------------------------------------
// Public API
// -----------------------------------------------------------------

int go_run_sandboxed(void* perms, const char* binary_path) {
    if (!binary_path) return -1;
    char* argv[] = { (char*)binary_path, NULL };
    return spawn_go_sandboxed(perms, binary_path, argv, 0);
}

int go_run(const char* binary_path) {
    return go_run_sandboxed(NULL, binary_path);
}

int go_run_script_sandboxed(void* perms, const char* script_path) {
    if (!script_path) return -1;
    const char* go_bin = get_go_path();
    char* argv[] = { (char*)go_bin, "run", (char*)script_path, NULL };
    return spawn_go_sandboxed(perms, go_bin, argv, 0);
}

int go_run_script(const char* script_path) {
    return go_run_script_sandboxed(NULL, script_path);
}

int go_run_sandboxed_with_map(void* perms, const char* binary_path, uint64_t map_token) {
    if (!binary_path) return -1;
    char* argv[] = { (char*)binary_path, NULL };
    return spawn_go_sandboxed(perms, binary_path, argv, map_token);
}

int go_run_script_sandboxed_with_map(void* perms, const char* script_path, uint64_t map_token) {
    if (!script_path) return -1;
    const char* go_bin = get_go_path();
    char* argv[] = { (char*)go_bin, "run", (char*)script_path, NULL };
    return spawn_go_sandboxed(perms, go_bin, argv, map_token);
}

#else

// Non-POSIX platforms (Windows): stubs that fail with a clear message.
int go_run_sandboxed(void* perms, const char* binary_path) {
    (void)perms; (void)binary_path;
    fprintf(stderr, "error: contrib.host.go requires POSIX (fork/exec/shm_open)\n");
    return -1;
}
int go_run_script_sandboxed(void* perms, const char* script_path) {
    (void)perms; (void)script_path;
    return go_run_sandboxed(NULL, NULL);
}
int go_run(const char* binary_path) {
    (void)binary_path;
    return go_run_sandboxed(NULL, NULL);
}
int go_run_script(const char* script_path) {
    (void)script_path;
    return go_run_sandboxed(NULL, NULL);
}
int go_run_sandboxed_with_map(void* perms, const char* binary_path, uint64_t map_token) {
    (void)perms; (void)binary_path; (void)map_token;
    return go_run_sandboxed(NULL, NULL);
}
int go_run_script_sandboxed_with_map(void* perms, const char* script_path, uint64_t map_token) {
    (void)perms; (void)script_path; (void)map_token;
    return go_run_sandboxed(NULL, NULL);
}

#endif
