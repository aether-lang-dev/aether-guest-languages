// test_support.c: what test_host_factor.ae needs that an Aether spec cannot
// say directly: a shared map by token (aether_shared_map_new hands its token
// back through an out-parameter), and reading it back.
#include <stddef.h>
#include <stdint.h>
#include "aether_shared_map.h"

static AetherSharedMap* g_map = NULL;
static uint64_t g_token = 0;

// The one map a spec needs, made on first use: its token.
int64_t ghl_t_map_new(void) {
    if (!g_map) g_map = aether_shared_map_new(&g_token);
    return (int64_t)g_token;
}

void ghl_t_map_put(const char* key, const char* value) {
    if (g_map) aether_shared_map_put(g_map, key, value);
}

// The value under `key`, or "" when there is none.
const char* ghl_t_map_get(const char* key) {
    const char* v = g_map ? aether_shared_map_get(g_map, key) : NULL;
    return v ? v : "";
}
