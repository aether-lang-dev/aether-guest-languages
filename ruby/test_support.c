// test_support.c: what test_host_ruby.ae needs that an Aether spec cannot say
// directly: shared maps by token (aether_shared_map_new hands its token back
// through an out-parameter), and a second thread using the bridge.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include "aether_shared_map.h"

int ruby_init_host(void);
int ruby_run(const char* code);

#define GHL_MAPS 8
static AetherSharedMap* g_maps[GHL_MAPS];
static uint64_t g_tokens[GHL_MAPS];

static AetherSharedMap* by_token(int64_t token) {
    for (int i = 0; i < GHL_MAPS; i++) {
        if (g_maps[i] && g_tokens[i] == (uint64_t)token) return g_maps[i];
    }
    return NULL;
}

// A new map: its token, or 0. The runtime makes a token from
// CLOCK_MONOTONIC's nanoseconds and a stack address, so two maps made within
// one tick of the clock get the same token; a map whose token is already in
// use is made again a tick later (as python/test_support.c does).
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
            aether_shared_map_free(m);
            usleep(10);
        }
        return 0;
    }
    return 0;
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

// Another thread asks the bridge for Ruby: libruby treats the thread that set
// it up as its main thread, so the bridge refuses (-1) instead of letting the
// VM crash. Both calls are made on the new thread.
static void* other_thread(void* out) {
    int a = ruby_init_host();
    int b = ruby_run("$ghl_other = 1");
    *(int*)out = (a == -1 && b == -1) ? 0 : 1;
    return NULL;
}

int ghl_t_other_thread_refused(void) {
    pthread_t t;
    int result = 9;
    if (pthread_create(&t, NULL, other_thread, &result) != 0) return -1;
    pthread_join(t, NULL);
    return result;
}
