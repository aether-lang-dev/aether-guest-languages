// test_support.c: what test_host_perl_host.ae needs that an Aether spec
// cannot say directly: shared maps by token (aether_shared_map_new hands
// its token back through an out-parameter) and two threads using the bridge
// at once.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "aether_shared_map.h"

// The bridge's own entry points (aether_host_perl.h).
int aether_perl_run(const char* code);
int aether_perl_register(const char* name, void* fn);

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

// Each thread runs 200 scripts through the bridge, one calling the spec's
// `add` host function (registered by the spec), at once. The interpreter is
// one, so the bridge takes turns. 0 when both threads saw every answer.
static void* thread_worker(void* arg) {
    long id = (long)arg;
    for (int i = 0; i < 200; i++) {
        char code[160];
        snprintf(code, sizeof(code),
            "die 'wrong' unless add(%d, %ld) == %ld; $main::t%ld = %d;", i, id, i + id, id, i);
        if (aether_perl_run(code) != 0) return (void*)1;
    }
    return NULL;
}

int ghl_t_two_threads(void) {
    pthread_t t1, t2;
    void *r1 = (void*)9, *r2 = (void*)9;
    if (pthread_create(&t1, NULL, thread_worker, (void*)1) != 0) return -1;
    if (pthread_create(&t2, NULL, thread_worker, (void*)1000) != 0) {
        pthread_join(t1, &r1);
        return -1;
    }
    pthread_join(t1, &r1);
    pthread_join(t2, &r2);
    return (r1 == NULL && r2 == NULL) ? 0 : 1;
}
