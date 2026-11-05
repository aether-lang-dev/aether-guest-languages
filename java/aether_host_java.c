// aether_host_java.c: the Java bridge's C half. New in aether-guest-languages:
// aether's contrib/host/java was a JVM-side jar and one Aether helper
// (grant_jvm_runtime) and started the JVM with spawn_sandboxed in the
// caller's code. This starts it, the way go/aether_host_go.c starts a Go
// program (and is that file, with `java` for `go`): a JVM is its own
// runtime and cannot share a process with another, so every call here starts
// a child, optionally with libaether_sandbox.so preloaded and the grants in
// AETHER_SANDBOX_SHM, which the agent (src/, -javaagent) reads too.
//
// The command is
//   $JAVA_BIN (else java on PATH) $AETHER_JAVA_FLAGS [-javaagent:$AETHER_JAVA_AGENT]
//       -cp <classpath> <main class>
// where the flags are separated by spaces (--enable-native-access=ALL-UNNAMED,
// and --enable-preview on JDK 21, where the Foreign Function API is a preview).
// The agent is added to sandboxed runs only, and only when the variable is set.
//
// Linux-only for real enforcement. LD_PRELOAD does not work under macOS System
// Integrity Protection, and there is no libaether_sandbox.so there: the
// sandboxed runs return -1 having started nothing, which is the safe answer.

#include "aether_host_java.h"

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
// Locate the JVM. Honour $JAVA_BIN if set, otherwise fall back to
// "java" (relying on $PATH at exec time).
// -----------------------------------------------------------------
static const char* get_java_path(void) {
    const char* env = getenv("JAVA_BIN");
    if (env && env[0]) return env;
    return "java";
}

#define MAX_JAVA_ARGS 64
// The command line: flags from $AETHER_JAVA_FLAGS (split on spaces; the copy
// is kept in `flags`), the agent, -cp and the main class. Returns argc, or -1.
static int java_argv(char** argv, char* flags, size_t flagsz, int sandboxed,
                     const char* classpath, const char* main_class, char* agent, size_t agentsz) {
    int n = 0;
    argv[n++] = (char*)get_java_path();
    const char* f = getenv("AETHER_JAVA_FLAGS");
    if (f && f[0]) {
        snprintf(flags, flagsz, "%s", f);
        for (char* tok = strtok(flags, " "); tok && n < MAX_JAVA_ARGS - 8; tok = strtok(NULL, " ")) argv[n++] = tok;
    }
    const char* ag = getenv("AETHER_JAVA_AGENT");
    if (sandboxed && ag && ag[0]) {
        snprintf(agent, agentsz, "-javaagent:%s", ag);
        argv[n++] = agent;
    }
    argv[n++] = (char*)"-cp";
    argv[n++] = (char*)classpath;
    argv[n++] = (char*)main_class;
    argv[n] = NULL;
    return n;
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
    snprintf(shm_name, 64, "/aether_host_java_%d", getpid());

    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { free(shm_name); return NULL; }

    ftruncate(fd, pos + 1);
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
// the parent so "java" works without an absolute path.
//
// Everything the child execs with is made here (#3200): the program's
// path, the /bin/sh argv for a file with no #! line (execvp ran those
// under /bin/sh), and the env with the preload, the grant source and
// the map put in. The child used to setenv them and call execvp between
// fork and exec, which a child of a threaded parent may not do: setenv
// mallocs and takes the environment's lock, and a lock another thread
// held at the fork stays held in the child. It now calls only execve.
// -----------------------------------------------------------------
static int spawn_java_sandboxed(void* perms,
                              const char* program,
                              char* const argv[],
                              uint64_t map_token) {
    if (!program || !argv) return -1;

    // Preload library.
    char preload_path[1024];
    if (perms) {
        char tried[2048];
        if (!aether_sandbox_find_preload(preload_path, sizeof(preload_path), tried, sizeof(tried))) {
            aether_sandbox_preload_missing("[java-host]", tried);
            return -1;
        }
    }

    // Grants → shm.
    char* grants_shm = NULL;
    if (perms) {
        grants_shm = serialize_grants_to_shm(perms);
        if (!grants_shm) {
            fprintf(stderr,
                    "[java-host] cannot serialize grants to shm\n");
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
                    "[java-host] cannot serialize map to shm\n");
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

static int run_java(void* perms, const char* classpath, const char* main_class, uint64_t map_token) {
    if (!classpath || !main_class) return -1;
    char* argv[MAX_JAVA_ARGS];
    char flags[1024], agent[1100];
    if (java_argv(argv, flags, sizeof flags, perms != NULL, classpath, main_class, agent, sizeof agent) < 0) return -1;
    return spawn_java_sandboxed(perms, argv[0], argv, map_token);
}

int java_run_sandboxed(void* perms, const char* classpath, const char* main_class) {
    return run_java(perms, classpath, main_class, 0);
}

int java_run(const char* classpath, const char* main_class) {
    return run_java(NULL, classpath, main_class, 0);
}

int java_run_sandboxed_with_map(void* perms, const char* classpath, const char* main_class, uint64_t map_token) {
    return run_java(perms, classpath, main_class, map_token);
}

#else

// Non-POSIX platforms (Windows): stubs that fail with a clear message.
int java_run_sandboxed(void* perms, const char* classpath, const char* main_class) {
    (void)perms; (void)classpath; (void)main_class;
    fprintf(stderr, "error: java requires POSIX (fork/exec/shm_open)\n");
    return -1;
}
int java_run(const char* classpath, const char* main_class) {
    return java_run_sandboxed(NULL, classpath, main_class);
}
int java_run_sandboxed_with_map(void* perms, const char* classpath, const char* main_class, uint64_t map_token) {
    (void)map_token;
    return java_run_sandboxed(perms, classpath, main_class);
}

#endif
