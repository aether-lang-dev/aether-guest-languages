// test_support.c: what test_host_python_vm.ae needs that an Aether spec
// cannot say directly: shared maps by token (aether_shared_map_new hands
// its token back through an out-parameter), two threads using the bridge at
// once, the process's resident size, and the address of the bridge's own
// run_sandboxed_with_map for a script to call back into (the nested map
// context case, which aether's harness found with dlsym on an -rdynamic
// binary).
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>
#include "aether_shared_map.h"

// The bridge's own entry points (aether_host_python.h).
void* python_vm_new(void);
void python_vm_free(void* vm);
int python_vm_register(void* vm, const char* name, void* host_fn);
void python_vm_set_global_int(void* vm, const char* name, int64_t value);
int python_vm_eval(void* vm, const char* code);
int64_t python_vm_result_int(void* vm);
void python_vm_pop_result(void* vm);
int python_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token);

#define GHL_MAPS 8
static AetherSharedMap* g_maps[GHL_MAPS];
static uint64_t g_tokens[GHL_MAPS];

static AetherSharedMap* by_token(int64_t token);

// A new map: its token, or 0. The runtime makes a token from
// CLOCK_MONOTONIC's nanoseconds and a stack address, so two maps made within
// one tick of the clock (a microsecond on macOS) get the same token, and a
// lookup by it finds the first. Until aether makes tokens that cannot
// collide, a map whose token is already in use is made again a tick later.
int64_t ghl_t_map_new(void) {
    for (int i = 0; i < GHL_MAPS; i++) {
        if (g_maps[i]) continue;
        for (int tries = 0; tries < 100; tries++) {
            uint64_t t = 0;
            AetherSharedMap* m = aether_shared_map_new(&t);
            if (!m) return 0;
            if (!by_token((int64_t)t)) {
                g_maps[i] = m;
                g_tokens[i] = t;
                return (int64_t)t;
            }
            // Not revoke_token(t): that finds the first map with t, the
            // one already in use. free takes this one off the registry.
            aether_shared_map_free(m);
            usleep(10);
        }
        return 0;
    }
    return 0;
}

static AetherSharedMap* by_token(int64_t token) {
    for (int i = 0; i < GHL_MAPS; i++) {
        if (g_maps[i] && g_tokens[i] == (uint64_t)token) return g_maps[i];
    }
    return NULL;
}

void ghl_t_map_put(int64_t token, const char* key, const char* value) {
    AetherSharedMap* m = by_token(token);
    if (m) aether_shared_map_put(m, key, value);
}

// The value under `key`, or "" when there is none.
const char* ghl_t_map_get(int64_t token, const char* key) {
    AetherSharedMap* m = by_token(token);
    const char* v = m ? aether_shared_map_get(m, key) : NULL;
    return v ? v : "";
}

void ghl_t_map_free(int64_t token) {
    for (int i = 0; i < GHL_MAPS; i++) {
        if (!g_maps[i] || g_tokens[i] != (uint64_t)token) continue;
        aether_shared_map_revoke_token(g_tokens[i]);
        aether_shared_map_free(g_maps[i]);
        g_maps[i] = NULL;
    }
}

// python_run_sandboxed_with_map's address, for ctypes.
int64_t ghl_t_run_with_map_addr(void) {
    return (int64_t)(intptr_t)&python_run_sandboxed_with_map;
}

// Each thread makes its own VM, registers `add` (the spec's cb_add) and
// calls it 300 times. 0 when both threads saw every answer.
static void* g_add;
static void* thread_worker(void* arg) {
    (void)arg;
    void* vm = python_vm_new();
    if (!vm) return (void*)1;
    python_vm_register(vm, "app.add", g_add);
    python_vm_set_global_int(vm, "N", 0);
    for (int i = 0; i < 300; i++) {
        char code[64];
        snprintf(code, sizeof(code), "app.add(%d, 23)", i);
        if (python_vm_eval(vm, code) != 0 || python_vm_result_int(vm) != i + 23) {
            python_vm_free(vm);
            return (void*)2;
        }
        python_vm_pop_result(vm);
    }
    python_vm_free(vm);
    return NULL;
}

int ghl_t_two_threads(void* add) {
    g_add = add;
    pthread_t t1, t2;
    void *r1 = (void*)9, *r2 = (void*)9;
    if (pthread_create(&t1, NULL, thread_worker, NULL) != 0) return -1;
    if (pthread_create(&t2, NULL, thread_worker, NULL) != 0) {
        pthread_join(t1, &r1);
        return -1;
    }
    pthread_join(t1, &r1);
    pthread_join(t2, &r2);
    return (r1 == NULL && r2 == NULL) ? 0 : 1;
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

int ghl_t_isinf(double d) { return isinf(d) ? 1 : 0; }
int64_t ghl_t_i64_max(void) { return INT64_MAX; }
int64_t ghl_t_i64_min(void) { return INT64_MIN; }
