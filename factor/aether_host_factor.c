// aether_host_factor.c: Embedded Factor Language Host Module (moved from
// aether's contrib.host.factor, PROVENANCE.md)
//
// Runs Factor code inside the Aether process via a *forked* libfactor
// (github.com/aether-lang-dev/factor-language, branch feat/embed-api) that
// exposes a generic, re-entrant C eval-with-result API the modern upstream
// VM lacks. The fork restores the old start_embedded_factor behaviour:
//   char* factor_embed_eval(const char* image, const char* src);
//   void  factor_embed_eval_free(char* result);
// init a persistent VM from a STOCK factor.image, mark it embedded so the
// image installs the eval>string callback, then evaluate `src` and RETURN
// the result string to the host (the VM survives — no process exit). The
// VM is reused across calls, so state persists between evals — which is
// what makes the k-v map (set/get) work.
//
// LOADING MODEL: dlopen, as the other bridges here: symbols resolved via
// dlsym at first call, so the program has no unresolved Factor symbols at
// link time and carries no DT_NEEDED libfactor (factor_abi.h has the types).
//
// SANDBOX: factor_run_sandboxed and factor_run_sandboxed_with_map push
// `perms` as a level of the calling thread's grants for the length of the
// call, as the lua and python bridges do. (In aether's copy they accepted
// `perms` and ignored it, on the ground that Factor's VM does its own GC,
// JIT (a writable and executable code heap), signal handling and threads,
// which the LD_PRELOAD libc gate does not "cleanly contain". The gate does
// hold Factor's libc calls once the VM is up -- io.files' open, FFI calls,
// dlopen -- as README.md's tables show; what it cannot hold is the VM's
// start, which maps the code heap executable and reads the image, so the
// VM is started before the level goes in force, and the code that runs
// under it can load nothing from disk.) The contained child (README.md,
// "A contained child") is the stronger answer, as for every language here.

#include "aether_host_factor.h"
// The installed runtime's headers (include/aether/runtime, and std/string),
// which `ae build` puts on the include path of every C file it compiles into
// a program. libaether is always built with the sandbox; aether's Makefile
// says so to its own C with -DAETHER_HAS_SANDBOX, which a program's build
// does not pass.
#ifndef AETHER_HAS_SANDBOX
#define AETHER_HAS_SANDBOX 1
#endif
#include "aether_sandbox.h"
#include "aether_shared_map.h"
#include "aether_string.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// In aether's tree this was defined by `make contrib` when the fork's
// libfactor was found. Here the declarations travel with the bridge
// (factor_abi.h), so it always builds; libfactor is still found only at run
// time.
#ifndef AETHER_HAS_FACTOR
#define AETHER_HAS_FACTOR 1
#endif

#if AETHER_HAS_FACTOR
#include "factor_abi.h"

// --- libfactor dlopen table -------------------------------------------------

static void* libfactor_handle = NULL;

static struct {
    // Generic re-entrant embedding API (fork vm/embed_api.cpp).
    factor_embed_eval_fn factor_embed_eval;
    factor_embed_eval_free_fn factor_embed_eval_free;
    // Host k-v hooks: register callbacks the embedded Factor code reaches via
    // FFI while it runs. Optional — absent on an older fork; the shared-map
    // entry point degrades gracefully if NULL.
    factor_embed_map_set_hooks_fn factor_embed_map_set_hooks;
} g_factor;

// Resolved Factor image path (the bootstrapped factor.image). The VM needs
// it; there is no system default the way liblua self-contains. Supplied by
// $AETHER_FACTOR_IMAGE, else NULL (lets the VM try its own default /
// embedded-image lookup).
static const char* g_factor_image = NULL;

static int resolve_factor_symbols(void* h) {
    *(void**)(&g_factor.factor_embed_eval) = dlsym(h, "factor_embed_eval");
    *(void**)(&g_factor.factor_embed_eval_free) =
        dlsym(h, "factor_embed_eval_free");
    if (!g_factor.factor_embed_eval || !g_factor.factor_embed_eval_free) {
        fprintf(stderr,
            "aether host_factor: libfactor missing factor_embed_eval / "
            "factor_embed_eval_free (needs the embed-api fork).\n");
        return -1;
    }
    // Optional: present on a fork with the k-v hooks. The shared-map run
    // path checks for it and errors clearly if the fork is too old.
    *(void**)(&g_factor.factor_embed_map_set_hooks) =
        dlsym(h, "factor_embed_map_set_hooks");
    return 0;
}

static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

// Load libfactor on first use. Two-step, orchestrator-owned probe (same
// posture as the lua bridge — no hardcoded version list):
//   1. $AETHER_FACTOR_SONAME (exact path/soname of the forked libfactor).
//   2. libfactor.so (unversioned, if installed on the host).
// The image path comes from $AETHER_FACTOR_IMAGE.
static int load_libfactor(void) {
    if (libfactor_handle) return 0;

    if (!g_factor_image) g_factor_image = getenv("AETHER_FACTOR_IMAGE");

    const char* env_soname = getenv("AETHER_FACTOR_SONAME");
    void* h = try_dlopen(env_soname);
    if (!h) h = try_dlopen("libfactor.so");
    if (!h) {
        fprintf(stderr,
            "aether host_factor: cannot dlopen libfactor "
            "(tried $AETHER_FACTOR_SONAME=%s, libfactor.so).\n"
            "  Build the embed-api fork's libfactor and set "
            "AETHER_FACTOR_SONAME to it, plus AETHER_FACTOR_IMAGE to the "
            "bootstrapped factor.image.\n  dlerror: %s\n",
            env_soname ? env_soname : "(unset)",
            dlerror() ? dlerror() : "(none)");
        return -1;
    }

    if (resolve_factor_symbols(h) != 0) {
        dlclose(h);
        return -1;
    }
    libfactor_handle = h;
    return 0;
}

int factor_init(void) {
    return load_libfactor();
}

void factor_finalize(void) {
    // The one-shot embed API owns VM lifecycle per call; nothing persistent
    // to tear down here yet. dlclose left to process exit (matches lua).
}

// Core: evaluate `code` in the persistent VM, return its captured output
// (eval>string) as a malloc'd C string, or NULL on failure. Caller frees
// via g_factor.factor_embed_eval_free. One trailing newline trimmed.
static char* factor_eval_raw(const char* code) {
    if (!code) return NULL;
    if (factor_init() != 0) return NULL;
    char* r = g_factor.factor_embed_eval(g_factor_image, code);
    if (r) {
        size_t n = strlen(r);
        if (n > 0 && r[n - 1] == '\n') r[n - 1] = '\0';
    }
    return r;
}

int factor_run(const char* code) {
    // Fire-and-forget: evaluate for effect, discard the captured output.
    char* r = factor_eval_raw(code);
    if (!r) return -1;
    g_factor.factor_embed_eval_free(r);
    return 0;
}

// Start the VM if it is not running, before a level goes in force: starting
// it reads the image and maps the code heap executable, which the preload
// would refuse. An empty eval does it. 0 when the VM is up.
static int g_factor_booted = 0;
static int factor_boot(void) {
    if (g_factor_booted) return 0;
    char* r = factor_eval_raw("");
    if (!r) return -1;
    g_factor.factor_embed_eval_free(r);
    g_factor_booted = 1;
    return 0;
}

int factor_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (factor_boot() != 0) return -1;
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;
    aether_sandbox_push(perms);
    int result = factor_run(code);
    aether_sandbox_pop();
    return result;
}

// --- First-class shared map (run_with_map), matching the other hosts -------
//
// Unlike scalar set/get, this hands the embedded Factor code a live view of
// an Aether-owned shared map: Aether builds the map, calls
// factor_run_sandboxed_with_map, the Factor script reads/writes it AS IT RUNS
// via two FFI words (aether-map-get / aether-map-put), and Aether reads the
// whole map back afterwards — including keys the script discovered at runtime
// (e.g. a word-frequency map-reduce). Aether enumerates with map.keys(); no
// host-side key-enumeration call is needed.
//
// Mechanism (as the tcl bridge's map_token bindings): the bridge
// registers C callbacks into the fork via factor_embed_map_set_hooks; those
// callbacks read/write the shared map by token. A small Factor FFI prelude
// (LIBRARY: factor + FUNCTION:) is prepended to the user code so the script
// can name aether-map-get / aether-map-put. Single-threaded: one token live
// at a time, set around the eval.

static uint64_t g_current_map_token = 0;

// Called FROM Factor (via the FFI prelude) to read a shared-map key. Returns
// a borrowed C string owned by the shared map; Factor copies it at the
// c-string return boundary, so the borrow only needs to outlive that copy.
static const char* host_factor_map_get(const char* key) {
    if (!g_current_map_token || !key) return NULL;
    return aether_shared_map_get_by_token(g_current_map_token, key);
}

// Called FROM Factor to write a shared-map key (output area).
static void host_factor_map_put(const char* key, const char* value) {
    if (!g_current_map_token || !key || !value) return;
    aether_shared_map_put_by_token(g_current_map_token, key, value);
}

// FFI prelude prepended to user code: declares the two host entry points as
// Factor words. Needs IN: (FUNCTION: defines words, which requires a
// vocabulary) and alien.syntax/alien.c-types for FUNCTION:/c-string/void.
// The wrapper words rename to the friendlier aether-map-get/put.
static const char* FACTOR_MAP_PRELUDE =
    "USING: alien.syntax alien.c-types ; IN: aether.host.map\n"
    "LIBRARY: factor\n"
    "FUNCTION: c-string factor_embed_map_get ( c-string key )\n"
    "FUNCTION: void factor_embed_map_put ( c-string key, c-string value )\n"
    ": aether-map-get ( key -- value/f ) factor_embed_map_get ;\n"
    ": aether-map-put ( key value -- ) factor_embed_map_put ;\n";

int factor_run_sandboxed_with_map(void* perms, const char* code,
                                  uint64_t map_token) {
    if (!code) return -1;
    if (factor_boot() != 0) return -1;
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;
    if (!g_factor.factor_embed_map_set_hooks) {
        fprintf(stderr,
            "aether host_factor: libfactor lacks factor_embed_map_set_hooks "
            "(needs a newer embed-api fork with the k-v hooks).\n");
        return -1;
    }

    // Inputs become read-only to the guest after this (matches lua/etc.).
    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);

    g_factor.factor_embed_map_set_hooks(host_factor_map_get,
                                        host_factor_map_put);
    g_current_map_token = map_token;

    // Prepend the FFI prelude so the script can call aether-map-get/put.
    size_t plen = strlen(FACTOR_MAP_PRELUDE);
    size_t clen = strlen(code);
    char* full = (char*)malloc(plen + clen + 2);
    if (!full) {
        g_current_map_token = 0;
        g_factor.factor_embed_map_set_hooks(NULL, NULL);
        return -1;
    }
    memcpy(full, FACTOR_MAP_PRELUDE, plen);
    full[plen] = '\n';
    memcpy(full + plen + 1, code, clen + 1);

    aether_sandbox_push(perms);
    char* r = factor_eval_raw(full);
    aether_sandbox_pop();
    free(full);

    g_current_map_token = 0;
    g_factor.factor_embed_map_set_hooks(NULL, NULL);

    if (!r) return -1;
    g_factor.factor_embed_eval_free(r);
    return 0;
}

// factor.eval(code) -> string. Evaluate `code` and return its captured
// output as an owned AetherString ("" on failure). The result comes back
// INTO Aether as a usable value (Rung B). Persistent VM, so state set by a
// prior eval is visible here.
AetherString* factor_eval(const char* code) {
    char* r = factor_eval_raw(code);
    if (!r) return string_new("");
    AetherString* out = string_new(r);
    g_factor.factor_embed_eval_free(r);
    return out;
}

// --- k-v map interop (built on the persistent VM, generic eval) -----------
//
// The fork's libfactor exposes only generic eval — no Aether-specific
// value model. set/get are layered HERE by synthesising Factor that writes
// to / reads from a global namespace, so the same persistent VM carries the
// map across calls (set in one call, the script reads/mutates it, get reads
// it back). Keys and string values are emitted as Factor string literals
// with `"` and `\` escaped so arbitrary content is safe.

// Append `s` to `buf` as the body of a Factor string literal (no quotes),
// escaping " and \. Returns chars written, or -1 on overflow.
static int append_escaped(char* buf, size_t cap, size_t at, const char* s) {
    size_t i = at;
    for (const char* p = s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (i + 2 >= cap) return -1;
            buf[i++] = '\\';
        } else if (i + 1 >= cap) return -1;
        buf[i++] = *p;
    }
    buf[i] = '\0';
    return (int)(i - at);
}

// factor.set(key, value) — store `value` (as a Factor string) under `key`
// in the global namespace of the persistent VM. Returns 0 on success.
int factor_set(const char* key, const char* value) {
    if (!key || !value) return -1;
    char snip[8192];
    // USING: namespaces ; "<value>" "<key>" set-global
    size_t at = 0;
    int w = snprintf(snip, sizeof(snip), "USING: namespaces ; \"");
    if (w < 0) return -1; at = (size_t)w;
    if (append_escaped(snip, sizeof(snip), at, value) < 0) return -1;
    at = strlen(snip);
    if (at + 4 >= sizeof(snip)) return -1;
    snip[at++] = '"'; snip[at++] = ' '; snip[at++] = '"'; snip[at] = '\0';
    if (append_escaped(snip, sizeof(snip), strlen(snip), key) < 0) return -1;
    at = strlen(snip);
    const char* tail = "\" set-global";
    if (at + strlen(tail) + 1 >= sizeof(snip)) return -1;
    memcpy(snip + at, tail, strlen(tail) + 1);

    char* r = factor_eval_raw(snip);
    if (!r) return -1;
    g_factor.factor_embed_eval_free(r);
    return 0;
}

// factor.get(key) -> string. Read `key` from the persistent VM's global
// namespace, rendered to a string. "" if absent/failed.
AetherString* factor_get(const char* key) {
    if (!key) return string_new("");
    char snip[8192];
    // USING: kernel namespaces present io ; "<key>" get [ present write ] when*
    // `present` renders the value as raw text — a string yields its own
    // content (no quotes), a number yields its decimal digits — matching the
    // string-only k-v convention of the other hosts (get "x" -> 10, not
    // "10"). Missing key -> "".
    int w = snprintf(snip, sizeof(snip),
        "USING: kernel namespaces present io ; \"");
    if (w < 0) return string_new("");
    if (append_escaped(snip, sizeof(snip), (size_t)w, key) < 0)
        return string_new("");
    size_t at = strlen(snip);
    const char* tail = "\" get [ present write ] when*";
    if (at + strlen(tail) + 1 >= sizeof(snip)) return string_new("");
    memcpy(snip + at, tail, strlen(tail) + 1);

    return factor_eval(snip);
}

#else /* !AETHER_HAS_FACTOR */

int factor_init(void) {
    fprintf(stderr, "aether host_factor: built without Factor support\n");
    return -1;
}
void factor_finalize(void) {}
int factor_run(const char* code) { (void)code; return factor_init(); }
int factor_run_sandboxed(void* perms, const char* code) {
    (void)perms; (void)code; return factor_init();
}
int factor_run_sandboxed_with_map(void* perms, const char* code,
                                  uint64_t map_token) {
    (void)perms; (void)code; (void)map_token; return factor_init();
}
AetherString* factor_eval(const char* code) {
    (void)code; (void)factor_init(); return string_new("");
}
int factor_set(const char* key, const char* value) {
    (void)key; (void)value; return factor_init();
}
AetherString* factor_get(const char* key) {
    (void)key; (void)factor_init(); return string_new("");
}

#endif /* AETHER_HAS_FACTOR */
