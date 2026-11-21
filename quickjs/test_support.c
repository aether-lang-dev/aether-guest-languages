// test_support.c: what test_host_quickjs_vm.ae needs that an Aether spec
// cannot say directly: a cancel cell set from another thread, the process's
// resident size, and the int64 limits. (tcl/test_support.c and
// python/test_support.c are the models.)
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

extern void aether_sync_atomic_store(void* cell, int64_t value);

typedef struct { void* cell; int ms; pthread_t th; } Later;

static void* set_later(void* p) {
    Later* l = (Later*)p;
    struct timespec ts = { l->ms / 1000, (long)(l->ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
    aether_sync_atomic_store(l->cell, 1);
    return NULL;
}

// Sets the std.sync cell to 1 from a new thread, ms milliseconds from now.
// ghl_t_join waits for that thread and frees what this returned.
void* ghl_t_cancel_later(void* cell, int ms) {
    Later* l = (Later*)calloc(1, sizeof(Later));
    if (!l) return NULL;
    l->cell = cell;
    l->ms = ms;
    if (pthread_create(&l->th, NULL, set_later, l) != 0) { free(l); return NULL; }
    return l;
}

void ghl_t_join(void* p) {
    Later* l = (Later*)p;
    if (!l) return;
    pthread_join(l->th, NULL);
    free(l);
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
int ghl_t_isinf(double d) { return isinf(d) ? 1 : 0; }
