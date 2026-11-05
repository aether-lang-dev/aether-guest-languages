// test_support.c: what test_host_tcl_vm.ae needs that an Aether spec cannot
// say directly: a VM used from a thread other than its own, and the
// process's resident size. aether's host_tcl_bidirectional harness did both
// inline.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>
#include "aether_host_tcl.h"

static void* other_thread(void* vm) {
    int rc = tcl_vm_eval(vm, "set x 1");
    static char out[200];
    snprintf(out, sizeof out, "%d|%d|%s", rc, tcl_vm_result_type(vm), tcl_vm_result_str(vm));
    return out;
}

// "<rc>|<result type>|<result string>" of an eval of `set x 1` made from a
// new thread, which is not the one that made the VM.
const char* ghl_t_eval_on_other_thread(void* vm) {
    pthread_t th;
    void* out = NULL;
    if (pthread_create(&th, NULL, other_thread, vm) != 0) return "pthread_create failed";
    pthread_join(th, &out);
    return (const char*)out;
}

// Resident size in kB: /proc where there is one, else the peak (macOS
// reports ru_maxrss in bytes, FreeBSD in kB).
int64_t ghl_t_rss_kb(void) {
    FILE* f = fopen("/proc/self/statm", "r");
    if (f) {
        long pages = 0, res = -1;
        if (fscanf(f, "%ld %ld", &pages, &res) != 2) res = -1;
        fclose(f);
        if (res >= 0) return res * (sysconf(_SC_PAGESIZE) / 1024);
    }
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return ru.ru_maxrss / 1024;
#else
    return ru.ru_maxrss;
#endif
}

int64_t ghl_t_i64_max(void) { return INT64_MAX; }
int64_t ghl_t_i64_min(void) { return INT64_MIN; }
