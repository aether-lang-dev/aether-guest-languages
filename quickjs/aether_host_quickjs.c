// aether_host_quickjs.c — Embedded QuickJS Host Module C Bridge
#include "aether_host_quickjs.h"
#ifndef AETHER_HAS_SANDBOX
#define AETHER_HAS_SANDBOX 1
#endif
// The installed runtime's headers (include/aether/runtime), which `ae build`
// puts on the include path of every C file it compiles into a program.
#include "aether_sandbox.h"
#include "aether_shared_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The engine is contrib.quickjs, which module.ae imports: its C (and the
// QuickJS amalgamation) is compiled into the same program, so it is always
// there. libaether is always built with the sandbox, which aether's Makefile
// says to its own C with a define a program's build does not pass.
#ifndef AETHER_HAS_QUICKJS
#define AETHER_HAS_QUICKJS 1
#endif

#if AETHER_HAS_QUICKJS

typedef struct { void *fn; void *env; } AeQjsClosure;

extern void* qjs_new(int memory_kb, int stack_kb);
extern void qjs_free(void* q);
extern void qjs_set_time_limit(void* q, int ms);
extern int qjs_eval(void* q, const char* code, const char* filename, int flags);
extern const char* qjs_error(void* q);
extern void qjs_release(void* q, int h);
extern int qjs_function(void *qp, const char *name, int arity, AeQjsClosure cb);
// Defined by contrib/quickjs/aether_quickjs.c, which a program gets by
// importing contrib.quickjs (this module's module.ae does). These must
// match those definitions exactly; there is no shared header.
extern int qjs_global(void *qp);
extern int qjs_object(void *qp);
extern int qjs_length(void *qp, int obj);
extern int qjs_set(void *qp, int obj, const char *name, int val);
extern int qjs_string(void *qp, const char *s);
extern int qjs_undefined(void *qp);
extern int qjs_get_index(void *qp, int obj, int i);
extern const char *qjs_to_string(void *qp, int h);

static void qjs_set_global_c(void* q, const char* name, int v) {
    int g = qjs_global(q);
    qjs_set(q, g, name, v);
    qjs_release(q, g);
}

static const char* qjs_arg_string_c(void* q, int args, int i) {
    int h = qjs_get_index(q, args, i);
    const char* s = qjs_to_string(q, h);
    qjs_release(q, h);
    return s ? s : "";
}

// print(...) and console.log(...): the arguments, space-separated, and a
// newline, on stdout. Not in aether's copy of this bridge (a script there
// could only compute and write to the shared map); added here so a script
// can say something. Nothing is checked: it writes to the process's own
// stdout, as Duktape's print does. In the contained child they are SAY.
static int quickjs_print_cb(void* env, void* q, int this_h, int args) {
    (void)env; (void)this_h;
    int n = qjs_length(q, args);
    for (int i = 0; i < n; i++) {
        if (i) fputc(' ', stdout);
        fputs(qjs_arg_string_c(q, args, i), stdout);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return qjs_undefined(q);
}

static void quickjs_install_print(void* q) {
    AeQjsClosure c = { (void*)quickjs_print_cb, NULL };
    int fn = qjs_function(q, "print", 0, c);
    qjs_set_global_c(q, "print", fn);
    int console = qjs_object(q);
    qjs_set(q, console, "log", fn);
    qjs_set_global_c(q, "console", console);
    qjs_release(q, console);
    qjs_release(q, fn);
}

int quickjs_init(void) {
    return 0;
}

void quickjs_finalize(void) {
}

int quickjs_run(const char* code) {
    if (!code) return -1;
    void* q = qjs_new(32768, 0);
    if (!q) return -1;
    qjs_set_time_limit(q, 5000);
    quickjs_install_print(q);
    int h = qjs_eval(q, code, "<eval>", 0);
    if (h < 0) {
        fprintf(stderr, "[js] %s\n", qjs_error(q));
        qjs_free(q);
        return -1;
    }
    qjs_release(q, h);
    qjs_free(q);
    return 0;
}

int quickjs_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;
    aether_sandbox_push(perms);
    int res = quickjs_run(code);
    aether_sandbox_pop();
    return res;
}

// Not in aether_shared_map.h; the Duktape host declares it the same way.
extern void aether_shared_map_freeze_inputs_by_token(uint64_t token);

static uint64_t quickjs_current_map_token = 0;

static int quickjs_map_get_cb(void* env, void* q, int this_h, int args) {
    (void)env; (void)this_h;
    const char* key = qjs_arg_string_c(q, args, 0);
    const char* val = aether_shared_map_get_by_token(quickjs_current_map_token, key);
    if (val) return qjs_string(q, val);
    return qjs_undefined(q);
}

static int quickjs_map_put_cb(void* env, void* q, int this_h, int args) {
    (void)env; (void)this_h;
    const char* key_tmp = qjs_arg_string_c(q, args, 0);
    char* key = strdup(key_tmp ? key_tmp : "");
    const char* val = qjs_arg_string_c(q, args, 1);
    aether_shared_map_put_by_token(quickjs_current_map_token, key, val);
    free(key);
    return qjs_undefined(q);
}

int quickjs_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    void* q = qjs_new(32768, 0);
    if (!q) return -1;
    qjs_set_time_limit(q, 5000);
    quickjs_install_print(q);

    aether_shared_map_freeze_inputs_by_token(map_token);
    quickjs_current_map_token = map_token;

    AeQjsClosure get_c = { (void*)quickjs_map_get_cb, NULL };
    int get_fn = qjs_function(q, "aether_map_get", 1, get_c);
    qjs_set_global_c(q, "aether_map_get", get_fn);
    qjs_release(q, get_fn);

    AeQjsClosure put_c = { (void*)quickjs_map_put_cb, NULL };
    int put_fn = qjs_function(q, "aether_map_put", 2, put_c);
    qjs_set_global_c(q, "aether_map_put", put_fn);
    qjs_release(q, put_fn);

    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) {
        qjs_free(q);
        quickjs_current_map_token = 0;
        return -1;
    }
    aether_sandbox_push(perms);
    int h = qjs_eval(q, code, "<eval>", 0);
    aether_sandbox_pop();
    quickjs_current_map_token = 0;

    if (h < 0) {
        fprintf(stderr, "[js] %s\n", qjs_error(q));
        qjs_free(q);
        return -1;
    }
    qjs_release(q, h);
    qjs_free(q);
    return 0;
}

#else
int quickjs_init(void) { return 0; }
void quickjs_finalize(void) {}
int quickjs_run(const char* code) { (void)code; return -1; }
int quickjs_run_sandboxed(void* perms, const char* code) { (void)perms; (void)code; return -1; }
int quickjs_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) { (void)perms; (void)code; (void)map_token; return -1; }
#endif
