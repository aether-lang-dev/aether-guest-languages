/* sanitize_options.c: linked into every program quickjs/sanitize.sh builds
 * (as an object in the link flags), never into an ordinary build.
 *
 * LeakSanitizer cannot start inside the contained child: at start it forks to
 * test ptrace, and the child's seccomp filter refuses the fork, which
 * LeakSanitizer takes as fatal, so the worker would not start at all. A
 * process under a seccomp filter (PR_GET_SECCOMP answers 2, or the call is
 * itself refused) therefore runs with AddressSanitizer only; every other
 * process, the hosts and an uncontained worker, keeps LeakSanitizer on.
 * A strong definition, so it wins over the sanitizer runtime's weak one. */
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
const char *__asan_default_options(void) {
    /* The raw system call: the runtime intercepts prctl(), and this runs
     * before it is ready. */
    long mode = syscall(SYS_prctl, PR_GET_SECCOMP, 0, 0, 0, 0);
    return (mode == 0) ? "detect_leaks=1" : "detect_leaks=0";
}
