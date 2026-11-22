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

// Keys and values arrive as Aether strings (the externs' @aether string) and
// cross by their length.
extern const char* aether_string_data(const void* s);
extern size_t aether_string_length(const void* s);
extern void* string_new_with_length(const char* data, size_t length);

void ghl_t_map_put(const void* key, const void* value) {
    const char* k = key ? aether_string_data(key) : NULL;
    const char* v = value ? aether_string_data(value) : NULL;
    if (g_map && k && v) aether_shared_map_put(g_map, k, aether_string_length(key), v, aether_string_length(value));
}

// The value under `key` as a new Aether string, or "" when there is none.
void* ghl_t_map_get(const void* key) {
    const char* k = key ? aether_string_data(key) : NULL;
    size_t vn = 0;
    const char* v = g_map && k ? aether_shared_map_get(g_map, k, aether_string_length(key), &vn) : NULL;
    return string_new_with_length(v ? v : "", v ? vn : 0);
}
