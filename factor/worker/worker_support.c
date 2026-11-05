// worker_support.c: for worker.ae. One pointer the worker's @c_callback
// shares (the std.guest.child Host), since a callback takes no state of its
// own; and the key-value hooks libfactor calls when the script asks for a
// host function (checks.factor's "the host's functions"), which turn each
// request into a call of that callback and hand its answer back.
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void* g_kept = 0;
void ghl_keep(void* p) { g_kept = p; }
void* ghl_kept(void) { return g_kept; }

// The Aether callback that makes the call, set by worker.ae.
static int (*g_call)(void*) = 0;
void ghl_call_set(void* cb) { g_call = (int (*)(void*))cb; }

// The request being made, and the answer to hand back ("KIND\001text").
static char g_name[256];
static char* g_args = 0;
static char* g_reply = 0;

const char* ghl_req_name(void) { return g_name; }
const char* ghl_req_args(void) { return g_args ? g_args : ""; }

void ghl_set_reply(const char* kind, const char* text) {
    free(g_reply);
    size_t n = strlen(kind) + strlen(text) + 2;
    g_reply = (char*)malloc(n);
    if (g_reply) snprintf(g_reply, n, "%s\001%s", kind, text);
}

// "\001name\001json-args" asks for the host function `name`; the answer is
// "KIND\001text", KIND being VALUE, FAIL or CANCEL (std.guest.child's Reply).
// Any other key is not a host function: the worker has no map of its own.
static const char* hook_get(const char* key) {
    if (!key || key[0] != 1 || !g_call) return NULL;
    const char* sep = strchr(key + 1, 1);
    if (!sep) return NULL;
    size_t n = (size_t)(sep - key - 1);
    if (n >= sizeof g_name) return NULL;
    memcpy(g_name, key + 1, n);
    g_name[n] = '\0';
    free(g_args);
    g_args = strdup(sep + 1);
    g_call(NULL);
    return g_reply;
}

static void hook_put(const char* key, const char* value) { (void)key; (void)value; }

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
