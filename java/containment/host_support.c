//go:build ignore
// (The line above keeps the go tool from compiling this file into checks.go's
// package; ae builds it.)

// host_support.c: the shared map, for host.ae. The runtime hands a new map
// its token through an out-parameter, which an extern cannot take; this
// keeps the one map a run needs and gives host.ae plain calls.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "aether_shared_map.h"

// Keys and values arrive as Aether strings (the externs' @aether string) and
// cross by their length: a NUL in either is stored and read back whole.
extern const char* aether_string_data(const void* s);
extern size_t aether_string_length(const void* s);
extern void* string_new_with_length(const char* data, size_t length);

static AetherSharedMap* g_map = NULL;
static uint64_t g_token = 0;

static const char* ghl_bytes(const void* s, size_t* n) {
    const char* d = s ? aether_string_data(s) : NULL;
    *n = d ? aether_string_length(s) : 0;
    return d ? d : "";
}

int64_t ghl_map_new(void) {
    if (!g_map) g_map = aether_shared_map_new(&g_token);
    return (int64_t)g_token;
}

void ghl_map_put(const void* key, const void* value) {
    size_t kn, vn;
    const char* k = ghl_bytes(key, &kn);
    const char* v = ghl_bytes(value, &vn);
    if (g_map) aether_shared_map_put(g_map, k, kn, v, vn);
}

// The value under `key` as a new Aether string, or "" when there is none.
void* ghl_map_get(const void* key) {
    size_t kn, vn = 0;
    const char* k = ghl_bytes(key, &kn);
    const char* v = g_map ? aether_shared_map_get(g_map, k, kn, &vn) : NULL;
    return string_new_with_length(v ? v : "", v ? vn : 0);
}

// A guest's write in the contained child, held to the rule the runtime's
// map holds an in-process guest to: the host's inputs are frozen. 1 when
// written, 0 when refused (the input, or a full map).
static int g_frozen_input = 1;
int ghl_map_put_guest(const void* key, const void* value) {
    if (!g_map) return 0;
    size_t kn, vn;
    const char* k = ghl_bytes(key, &kn);
    const char* v = ghl_bytes(value, &vn);
    if (g_frozen_input && kn == 5 && memcmp(k, "input", 5) == 0) return 0;
    return aether_shared_map_put(g_map, k, kn, v, vn) == 0 ? 1 : 0;
}
