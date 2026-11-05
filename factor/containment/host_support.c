// host_support.c: for host.ae and child_host.ae. The shared map: the runtime
// hands a new map its token through an out-parameter, which an extern cannot
// take; this keeps the one map a run needs and gives the hosts plain calls.
// And, for host.ae only, the host functions of the rows that call one
// (binding-*, callback-*), answered in C over the key-value hooks of
// libfactor, the only channel the embedding has (checks.factor's
// "the host's functions").
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#ifndef AETHER_HAS_SANDBOX
#define AETHER_HAS_SANDBOX 1
#endif
#include "aether_sandbox.h"
// 1 when every level in force grants it (runtime/aether_sandbox.h).
extern int aether_sandbox_check_slow(const char* category, const char* resource);
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

// ---- host functions, in-process ---------------------------------------------

// The reply under construction, valid until the next call: "KIND\001text".
static char* g_reply = NULL;

static const char* reply(const char* kind, const char* text) {
    free(g_reply);
    size_t n = strlen(kind) + strlen(text) + 2;
    g_reply = (char*)malloc(n);
    if (!g_reply) return NULL;
    snprintf(g_reply, n, "%s\001%s", kind, text);
    return g_reply;
}

// `s` as a JSON string literal, in a new buffer.
static char* json_string(const char* s) {
    size_t n = strlen(s);
    char* out = (char*)malloc(n * 6 + 3);
    char* p = out;
    if (!out) return NULL;
    *p++ = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { *p++ = '\\'; *p++ = (char)c; }
        else if (c == '\n') { *p++ = '\\'; *p++ = 'n'; }
        else if (c < 32) p += sprintf(p, "\\u%04x", c);
        else *p++ = (char)c;
    }
    *p++ = '"';
    *p = '\0';
    return out;
}

static const char* value_string(const char* s) {
    char* j = json_string(s);
    const char* r = reply("VALUE", j ? j : "\"\"");
    free(j);
    return r;
}

// The one string argument of a one-argument JSON call ["x"], without escapes.
static int one_arg(const char* args, char* out, size_t cap) {
    const char* a = strchr(args, '"');
    const char* b = strrchr(args, '"');
    if (!a || b <= a || (size_t)(b - a) >= cap) return 0;
    memcpy(out, a + 1, (size_t)(b - a - 1));
    out[b - a - 1] = '\0';
    return 1;
}

static const char* serve(const char* name, const char* args) {
    char arg[4096];
    if (strcmp(name, "read_file") == 0 && one_arg(args, arg, sizeof arg)) {
        if (!aether_sandbox_check_slow("fs_read", arg)) {
            char m[4200];
            snprintf(m, sizeof m, "refused: fs_read %s", arg);
            return reply("FAIL", m);
        }
        FILE* f = fopen(arg, "r");
        if (!f) return reply("FAIL", strerror(errno));
        char buf[4096];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = '\0';
        return value_string(buf);
    }
    if (strcmp(name, "getenv") == 0 && one_arg(args, arg, sizeof arg)) {
        if (!aether_sandbox_check_slow("env", arg)) {
            char m[4200];
            snprintf(m, sizeof m, "refused: env %s", arg);
            return reply("FAIL", m);
        }
        const char* v = getenv(arg);
        if (!v) {
            char m[4200];
            snprintf(m, sizeof m, "%s is unset", arg);
            return reply("FAIL", m);
        }
        return value_string(v);
    }
    if (strcmp(name, "greet") == 0 && one_arg(args, arg, sizeof arg)) {
        char m[4200];
        snprintf(m, sizeof m, "hello, %s", arg);
        return value_string(m);
    }
    if (strcmp(name, "add") == 0) {
        long x, y;
        if (sscanf(args, "[%ld,%ld]", &x, &y) == 2) {
            char m[64];
            snprintf(m, sizeof m, "%ld", x + y);
            return reply("VALUE", m);
        }
        return reply("FAIL", "add takes two ints");
    }
    if (strcmp(name, "__line") == 0 && one_arg(args, arg, sizeof arg)) {
        // The row's verdict line (checks.factor's, a JSON string with escapes
        // for the quotes it holds): stored for the host to read.
        const char* a = strchr(args, '"');
        const char* b = strrchr(args, '"');
        char* line = (char*)malloc((size_t)(b - a) + 1);
        char* p = line;
        for (const char* q = a + 1; q < b; q++) {
            if (*q == '\\' && q + 1 < b) { q++; *p++ = (*q == 'n') ? '\n' : *q; }
            else *p++ = *q;
        }
        *p = '\0';
        ghl_map_put("line", line);
        free(line);
        return reply("VALUE", "true");
    }
    char m[4200];
    snprintf(m, sizeof m, "no host function %s", name);
    return reply("FAIL", m);
}

// get hook: "\001name\001json-args" asks for a host function; any other key
// is the shared map's.
static const char* hook_get(const char* key) {
    if (!key) return NULL;
    if (key[0] == 1) {
        const char* sep = strchr(key + 1, 1);
        if (!sep) return reply("FAIL", "malformed call");
        char name[128];
        size_t n = (size_t)(sep - key - 1);
        if (n >= sizeof name) return reply("FAIL", "name too long");
        memcpy(name, key + 1, n);
        name[n] = '\0';
        return serve(name, sep + 1);
    }
    const char* v = g_map ? aether_shared_map_get(g_map, key) : NULL;
    return v;
}

static void hook_put(const char* key, const char* value) {
    if (g_map && key && value) aether_shared_map_put(g_map, key, value);
}

// Registers the hooks with libfactor, which the bridge has dlopen'd
// RTLD_GLOBAL by now. 1 when it could.
int ghl_hooks_install(void) {
    void (*set_hooks)(const char* (*)(const char*), void (*)(const char*, const char*)) =
        (void (*)(const char* (*)(const char*), void (*)(const char*, const char*)))
        dlsym(RTLD_DEFAULT, "factor_embed_map_set_hooks");
    if (!set_hooks) return 0;
    set_hooks(hook_get, hook_put);
    return 1;
}
