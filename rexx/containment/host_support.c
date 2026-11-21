// host_support.c: the shared map, for host.ae. The runtime hands a new map
// its token through an out-parameter, which an extern cannot take; this
// keeps the one map a run needs and gives host.ae plain calls.
#include <stdint.h>
#include <stddef.h>
#include "aether_shared_map.h"

static AetherSharedMap* g_map = NULL;
static uint64_t g_token = 0;

int64_t ghl_map_new(void) {
    if (!g_map) g_map = aether_shared_map_new(&g_token);
    return (int64_t)g_token;
}

void ghl_map_put(const char* key, const char* value) {
    if (g_map) aether_shared_map_put(g_map, key, value);
}

// The value under `key`, or "" when there is none.
const char* ghl_map_get(const char* key) {
    const char* v = g_map ? aether_shared_map_get(g_map, key) : NULL;
    return v ? v : "";
}

// A guest's write in the contained child, held to the rule the runtime's
// map holds an in-process guest to: the host's inputs are frozen. 1 when
// written, 0 when refused.
static int g_frozen_input = 1;
int ghl_map_put_guest(const char* key, const char* value) {
    if (!g_map) return 0;
    if (g_frozen_input && key && key[0] == 'i' && __builtin_strcmp(key, "input") == 0) return 0;
    aether_shared_map_put(g_map, key, value);
    return 1;
}

// The row exec-bin-sh runs `/bin/sh` through the bridge's exec(), which is
// system(): a shell that reads its standard input. Point the host's at
// /dev/null so it reads end of file and exits, whatever started the host.
#include <fcntl.h>
#include <unistd.h>
void ghl_stdin_null(void) {
    int fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) { dup2(fd, 0); if (fd != 0) close(fd); }
}
