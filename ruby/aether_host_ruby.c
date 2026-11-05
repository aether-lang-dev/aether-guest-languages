// aether_host_ruby.c: Embedded Ruby Language Host Module
//
// Embeds Ruby (CRuby/MRI) in the Aether process. When run_sandboxed is
// called, pushes its grants as a level of the calling thread's (#2955), so
// Ruby's libc calls (File.open, system, sockets, Fiddle's dlopen) are checked
// against the grant list when libaether_sandbox.so is preloaded.
//
// LOADING MODEL: dlopen, not -lruby. Every Ruby C-API symbol is resolved with
// dlsym when libruby is first needed, so the bridge has no unresolved Ruby
// symbols, binaries have no DT_NEEDED libruby, and they run against whatever
// Ruby 3 the deploy host has. The few types the bridge needs are declared in
// ruby_abi.h: no ruby.h, no Ruby dev package, to build.
//
// WHAT RUBY ASKS OF AN EMBEDDER (ruby/README.md has the long form):
//
//  - One interpreter per process, set up once on one thread. ruby_setup()
//    cannot be undone and redone: after ruby_finalize_host() the bridge
//    refuses to start Ruby again (aether's copy returned 0 and crashed on
//    the next eval). Calls from any thread but the one that set Ruby up are
//    refused, since libruby treats it as its main thread.
//
//  - The VM must be told where the C stack starts, because the GC scans the
//    C stack for VALUEs. RUBY_INIT_STACK is a macro over ruby_init_stack(
//    &local); a local in whichever frame happened to start Ruby is too low
//    for a later call from a shallower frame, whose VALUEs the GC would then
//    not scan. The bridge passes the real top of the calling thread's stack
//    (pthread_getattr_np / pthread_get_stackaddr_np), so every later call is
//    covered, and falls back to a local when the system will not say.
//
//  - Qnil, Qtrue and Qfalse are not exported symbols and differ between
//    Rubies (nil is 0x08 until 3.3 and 0x04 from 3.4; flonum builds differ
//    again). They are read from the loaded interpreter, by evaluating `nil`,
//    `true` and `false` once at init (ruby_abi.h).
//
//  - Anything that raises must not cross an Aether frame: a longjmp out of
//    a host callback would skip its defers. Every conversion a callback can
//    ask for that may raise (to_s, a big integer to 64 bits) runs under
//    rb_protect, and a callback's own errors are raised from the C method
//    after the callback has returned.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   // pthread_getattr_np
#endif
#include "aether_host_ruby.h"
// The installed runtime's headers (include/aether/runtime), which `ae build`
// puts on the include path of every C file it compiles into a program.
// libaether is always built with the sandbox; aether's Makefile says so to
// its own C with -DAETHER_HAS_SANDBOX, which a program's build does not pass.
#ifndef AETHER_HAS_SANDBOX
#define AETHER_HAS_SANDBOX 1
#endif
#include "aether_sandbox.h"
#include "aether_shared_map.h"

// In aether's tree this was defined by `make contrib` when a Ruby dev kit
// was found. Here the declarations travel with the bridge (ruby_abi.h), so it
// always builds; libruby is still found only at run time.
#ifndef AETHER_HAS_RUBY
#define AETHER_HAS_RUBY 1
#endif

#if AETHER_HAS_RUBY
#include "ruby_abi.h"
#include <dlfcn.h>
#include <pthread.h>
#if defined(__FreeBSD__)
#include <pthread_np.h>   // pthread_attr_get_np
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AE_RB_TNIL   0
#define AE_RB_TBOOL  1
#define AE_RB_TINT   2
#define AE_RB_TSTR   3
#define AE_RB_TOTHER 6
#define AE_RB_TFLOAT 8

// --- libruby dlopen table ---------------------------------------------------

static void* libruby_handle = NULL;

static struct {
    // Lifecycle.
    int   (*ruby_setup)(void);
    void* (*ruby_options)(int, char**);
    void  (*ruby_init_stack)(volatile VALUE* addr);
    int   (*ruby_cleanup)(int);
    // Eval and errors.
    VALUE (*rb_eval_string_protect)(const char*, int*);
    VALUE (*rb_protect)(VALUE (*)(VALUE), VALUE, int*);
    VALUE (*rb_errinfo)(void);
    void  (*rb_set_errinfo)(VALUE);
    void  (*rb_exc_raise)(VALUE);
    VALUE (*rb_exc_new)(VALUE klass, const char* ptr, long len);
    // The instruction limit: a TracePoint written in C.
    VALUE (*rb_tracepoint_new)(VALUE target_thread, uint32_t events, void (*func)(VALUE, void*), void* data);
    VALUE (*rb_tracepoint_enable)(VALUE);
    VALUE (*rb_tracepoint_disable)(VALUE);
    void  (*rb_gc_register_address)(VALUE*);
    // rb_intern and rb_funcall are macros in ruby.h (rb_intern with a
    // constant string, rb_funcall a GCC statement expression), so a field of
    // that name would be rewritten by the preprocessor. The headers are not
    // included any more, but the f_ prefix keeps the names recognisable.
    ID    (*f_rb_intern)(const char*);
    VALUE (*f_rb_funcall)(VALUE, ID, int, ...);
    // Strings and numbers.
    VALUE (*rb_obj_as_string)(VALUE);
    char* (*rb_string_value_ptr)(volatile VALUE*);
    VALUE (*rb_utf8_str_new)(const char*, long);
    VALUE (*rb_ll2inum)(long long);
    long long (*rb_num2ll)(VALUE);
    VALUE (*rb_float_new)(double);
    double (*rb_num2dbl)(VALUE);
    VALUE (*rb_obj_class)(VALUE);
    VALUE (*rb_class_name)(VALUE);
    // Methods.
    void  (*rb_define_global_function)(const char*, void*, int);
    // Exported data: the classes the bridge tells values apart by.
    VALUE* rb_cInteger;
    VALUE* rb_cString;
    VALUE* rb_cFloat;
    VALUE* rb_eRuntimeError;
} g_rb;

// nil, true and false as the LOADED libruby spells them (ruby_abi.h). Set in
// ruby_init_host, by evaluating them, before anything compares against them.
static VALUE bridge_qnil = 0;
static VALUE bridge_qtrue = 0;
static VALUE bridge_qfalse = 0;

static int resolve_ruby_symbols(void* h) {
#define RESOLVE(field, sym) do {                                       \
        *(void**)(&g_rb.field) = dlsym(h, sym);                        \
        if (!g_rb.field) {                                             \
            fprintf(stderr,                                            \
                "aether host_ruby: libruby missing symbol %s\n", sym); \
            return -1;                                                 \
        }                                                              \
    } while (0)

    RESOLVE(ruby_setup,                 "ruby_setup");
    RESOLVE(ruby_options,               "ruby_options");
    RESOLVE(ruby_init_stack,            "ruby_init_stack");
    RESOLVE(ruby_cleanup,               "ruby_cleanup");
    RESOLVE(rb_eval_string_protect,     "rb_eval_string_protect");
    RESOLVE(rb_protect,                 "rb_protect");
    RESOLVE(rb_errinfo,                 "rb_errinfo");
    RESOLVE(rb_set_errinfo,             "rb_set_errinfo");
    RESOLVE(rb_exc_raise,               "rb_exc_raise");
    RESOLVE(rb_exc_new,                 "rb_exc_new");
    RESOLVE(rb_tracepoint_new,          "rb_tracepoint_new");
    RESOLVE(rb_tracepoint_enable,       "rb_tracepoint_enable");
    RESOLVE(rb_tracepoint_disable,      "rb_tracepoint_disable");
    RESOLVE(rb_gc_register_address,     "rb_gc_register_address");
    RESOLVE(f_rb_intern,                "rb_intern");
    RESOLVE(f_rb_funcall,               "rb_funcall");
    RESOLVE(rb_obj_as_string,           "rb_obj_as_string");
    RESOLVE(rb_string_value_ptr,        "rb_string_value_ptr");
    RESOLVE(rb_utf8_str_new,            "rb_utf8_str_new");
    RESOLVE(rb_ll2inum,                 "rb_ll2inum");
    RESOLVE(rb_num2ll,                  "rb_num2ll");
    RESOLVE(rb_float_new,               "rb_float_new");
    RESOLVE(rb_num2dbl,                 "rb_num2dbl");
    RESOLVE(rb_obj_class,               "rb_obj_class");
    RESOLVE(rb_class_name,              "rb_class_name");
    RESOLVE(rb_define_global_function,  "rb_define_global_function");
    RESOLVE(rb_cInteger,                "rb_cInteger");
    RESOLVE(rb_cString,                 "rb_cString");
    RESOLVE(rb_cFloat,                  "rb_cFloat");
    RESOLVE(rb_eRuntimeError,           "rb_eRuntimeError");
    return 0;
#undef RESOLVE
}

static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

// Load libruby on first use. Strict two-step contract:
//   1. ${AETHER_RUBY_SONAME}: an exact soname or path, from the orchestrator
//      (tests/ruby_env.sh finds it from `ruby -rrbconfig`).
//   2. libruby.so, the unversioned name (Debian's ruby-dev, Arch, Fedora).
// No list of versions to try: the program that starts yours owns that.
// RTLD_GLOBAL, so the C extensions Ruby loads later (socket, fiddle, json)
// resolve their rb_* symbols against this libruby and not a second copy.
// Returns 0 on success, -1 on any failure.
static int load_libruby(void) {
    if (libruby_handle) return 0;

    const char* env_soname = getenv("AETHER_RUBY_SONAME");
    void* h = try_dlopen(env_soname);
    // dlerror() is read once and kept: it returns the message of the last
    // failure only the first time it is called after it.
    const char* first_err = h ? NULL : dlerror();
    char first_msg[512];
    snprintf(first_msg, sizeof first_msg, "%s", first_err ? first_err : "");
    if (!h) h = try_dlopen("libruby.so");
    if (!h) {
        const char* last_err = dlerror();
        fprintf(stderr,
            "aether host_ruby: cannot dlopen libruby "
            "(tried $AETHER_RUBY_SONAME=%s, libruby.so).\n"
            "  Install a ruby3 runtime on the host, or set "
            "AETHER_RUBY_SONAME to the exact soname or path.\n"
            "  Hint: $(ruby -rrbconfig -e "
            "'puts File.join(RbConfig::CONFIG[\"libdir\"], RbConfig::CONFIG[\"LIBRUBY_SO\"])')\n"
            "  dlerror: %s\n",
            env_soname ? env_soname : "(unset)",
            first_msg[0] ? first_msg : (last_err ? last_err : "(none)"));
        return -1;
    }

    if (resolve_ruby_symbols(h) != 0) {
        dlclose(h);
        return -1;
    }
    libruby_handle = h;
    return 0;
}

// --- the grant list ---------------------------------------------------------

extern int list_size(void*);
extern void* list_get_raw(void*, int);
/* A grant list std.sandbox built holds plain C strings; one a program
 * built can hold Aether strings (a helper that keeps its `string`
 * parameter stores its own copy). This reads the bytes of either. */
extern const char* aether_string_data(const void*);

// --- lifecycle ---------------------------------------------------------------

static int ruby_initialized = 0;
static int ruby_finalized = 0;
static pthread_t ruby_thread;

// The highest address of the calling thread's stack, or NULL when the system
// will not say (the caller then falls back to a local's address).
static void* stack_top(void) {
#if defined(__linux__) || defined(__FreeBSD__)
    pthread_attr_t attr;
    void* addr = NULL;
    size_t size = 0;
#if defined(__linux__)
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return NULL;
#else
    pthread_attr_init(&attr);
    if (pthread_attr_get_np(pthread_self(), &attr) != 0) { pthread_attr_destroy(&attr); return NULL; }
#endif
    if (pthread_attr_getstack(&attr, &addr, &size) != 0) addr = NULL; else addr = (char*)addr + size;
    pthread_attr_destroy(&attr);
    return addr;
#elif defined(__APPLE__)
    return pthread_get_stackaddr_np(pthread_self());
#else
    return NULL;
#endif
}

// Evaluates `src` and returns the VALUE, or 0 with *ok = 0.
static VALUE eval_quiet(const char* src, int* ok) {
    int st = 0;
    VALUE v = g_rb.rb_eval_string_protect(src, &st);
    *ok = !st;
    if (st) g_rb.rb_set_errinfo(bridge_qnil);
    return v;
}

int ruby_init_host(void) {
    if (ruby_initialized) {
        if (!pthread_equal(pthread_self(), ruby_thread)) {
            fprintf(stderr, "aether host_ruby: Ruby was set up on another thread and can only be used from it\n");
            return -1;
        }
        return 0;
    }
    if (ruby_finalized) {
        fprintf(stderr, "aether host_ruby: Ruby was finalized; ruby_setup cannot run twice in a process\n");
        return -1;
    }
    if (load_libruby() != 0) return -1;

    // RUBY_INIT_STACK, as a call (see the header comment): the real top of
    // this thread's stack, so a later call from any frame is below it.
    volatile VALUE local_bottom = 0;
    void* top = stack_top();
    g_rb.ruby_init_stack(top ? (volatile VALUE*)((char*)top - 256) : &local_bottom);

    // ruby_setup, not ruby_init: ruby_init exits the process when setup fails.
    int state = g_rb.ruby_setup();
    if (state != 0) {
        fprintf(stderr, "aether host_ruby: ruby_setup failed (state %d)\n", state);
        return -1;
    }
    ruby_initialized = 1;
    ruby_thread = pthread_self();
    // ruby_setup alone leaves out the half of the standard library that
    // Ruby 3.3 and later write in Ruby itself (Integer#times, Kernel#Integer,
    // GC.start, Array#each ...: "undefined method" on the first call): those
    // "builtin" methods are loaded by ruby_options, not ruby_setup. This is
    // main.c's sequence (ruby_init, then ruby_options) with a script that
    // does nothing; ruby_options also sets up the load path. RubyGems is
    // left out (it is the larger part of Ruby's start-up, and needs gem
    // directories the process may not be allowed to read) and so is $RUBYOPT,
    // which would change how an embedded Ruby behaves from one host to the
    // next. A script can `require 'rubygems'`, and AETHER_RUBY_GEMS=1 loads
    // it at start: needed to `require` a bundled gem, which from Ruby 3.5 is
    // what fiddle is.
    {
        const char* gems = getenv("AETHER_RUBY_GEMS");
        int use_gems = gems && *gems && strcmp(gems, "0") != 0;
        char a0[] = "ruby", a1[] = "--disable-gems", a2[] = "--disable-rubyopt", a3[] = "-e", a4[] = "nil";
        char* argv_nogems[] = { a0, a1, a2, a3, a4, NULL };
        char* argv_gems[] = { a0, a2, a3, a4, NULL };
        if (use_gems) g_rb.ruby_options(4, argv_gems);
        else g_rb.ruby_options(5, argv_nogems);
    }

    // Ask the interpreter what nil, true and false are, now that it can
    // answer. Must precede any eval that inspects errinfo: the first failure
    // would otherwise compare against, and assign, the wrong constant.
    int ok = 0;
    bridge_qnil = eval_quiet("nil", &ok);
    if (!ok) goto fail;
    bridge_qtrue = eval_quiet("true", &ok);
    if (!ok) goto fail;
    bridge_qfalse = eval_quiet("false", &ok);
    if (!ok) goto fail;
    // Unbuffered, so what a script prints is there when the program exits:
    // nothing flushes $stdout at exit (aether's copy lost it).
    eval_quiet("$stdout.sync = true; $stderr.sync = true; nil", &ok);
    return 0;
fail:
    fprintf(stderr, "aether host_ruby: cannot evaluate nil/true/false after ruby_setup\n");
    return -1;
}

void ruby_finalize_host(void) {
    if (!ruby_initialized) return;
    if (!pthread_equal(pthread_self(), ruby_thread)) return;
    g_rb.ruby_cleanup(0);
    ruby_initialized = 0;
    ruby_finalized = 1;
}

// --- errors -------------------------------------------------------------------

static VALUE message_thunk(VALUE e) {
    return g_rb.f_rb_funcall(e, g_rb.f_rb_intern("message"), 0);
}

static VALUE as_string_thunk(VALUE v) {
    return g_rb.rb_obj_as_string(v);
}

// A String's bytes. A conversion that may raise runs under rb_protect, and
// gives "" when it does.
static const char* string_ptr(VALUE v) {
    volatile VALUE s = v;
    if (g_rb.rb_obj_class(s) != *g_rb.rb_cString) {
        int st = 0;
        s = g_rb.rb_protect(as_string_thunk, s, &st);
        if (st) { g_rb.rb_set_errinfo(bridge_qnil); return ""; }
    }
    return g_rb.rb_string_value_ptr(&s);
}

// The pending exception's message, or NULL when there is none. With_class,
// "Class: message". The result is malloc'd.
static char* take_error(int with_class) {
    VALUE err = g_rb.rb_errinfo();
    if (err == bridge_qnil) return NULL;
    int st = 0;
    volatile VALUE msg = g_rb.rb_protect(message_thunk, err, &st);
    char* out = NULL;
    if (st) {
        out = strdup("(no message)");
    } else {
        const char* m = g_rb.rb_string_value_ptr(&msg);
        if (with_class) {
            volatile VALUE cn = g_rb.rb_class_name(g_rb.rb_obj_class(err));
            const char* c = g_rb.rb_string_value_ptr(&cn);
            size_t n = strlen(c) + strlen(m) + 3;
            out = (char*)malloc(n);
            if (out) snprintf(out, n, "%s: %s", c, m);
        } else {
            out = strdup(m);
        }
    }
    g_rb.rb_set_errinfo(bridge_qnil);
    return out;
}

// Safe eval: catches exceptions and prints them.
static int eval_ruby(const char* code) {
    int state = 0;
    g_rb.rb_eval_string_protect(code, &state);
    if (state) {
        char* m = take_error(0);
        if (m) {
            fprintf(stderr, "[ruby] %s\n", m);
            free(m);
        }
        g_rb.rb_set_errinfo(bridge_qnil);
        return -1;
    }
    return 0;
}

// --- sandbox / permission stack ----------------------------------------------

// Appends `s` to `out` as the inside of a single-quoted Ruby string.
static size_t put_quoted(char* out, size_t cap, size_t pos, const char* s) {
    for (; *s && pos + 2 < cap; s++) {
        if (*s == '\'' || *s == '\\') out[pos++] = '\\';
        out[pos++] = *s;
    }
    out[pos] = 0;
    return pos;
}

// Ruby's ENV reads libc's environment on every access (it is not a copy
// taken at start, as Perl's %ENV and Python's os.environ are), so what a
// script may see is what is in the process's environment. Delete the
// variables the grants do not name, run, and put the whole environment back.
// (aether's copy deleted them for good: the Aether program that called
// run_sandboxed lost its own environment.) The scrub is the bridge's own
// housekeeping, so it runs before the grants are pushed and the restore
// after they are popped.
static void env_scrub(void* perms) {
    int n = list_size(perms);
    char scrub[8192];
    int pos = snprintf(scrub, sizeof(scrub),
        "($__aether_env ||= []).push(ENV.to_h); _keep = {}; ");
    for (int i = 0; i + 1 < n && pos < 7000; i += 2) {
        const char* cat = aether_string_data(list_get_raw(perms, i));
        const char* pat = aether_string_data(list_get_raw(perms, i + 1));
        if (cat && strcmp(cat, "env") == 0 && pat) {
            if (strcmp(pat, "*") == 0) {
                pos += snprintf(scrub + pos, sizeof(scrub) - pos,
                    "ENV.each_key { |k| _keep[k] = true }; ");
            } else {
                pos += snprintf(scrub + pos, sizeof(scrub) - pos, "_keep['");
                pos = (int)put_quoted(scrub, sizeof(scrub) - 64, pos, pat);
                pos += snprintf(scrub + pos, sizeof(scrub) - pos, "'] = true; ");
            }
        }
    }
    snprintf(scrub + pos, sizeof(scrub) - pos,
        "ENV.keys.each { |k| ENV.delete(k) unless _keep[k] }; nil");
    eval_ruby(scrub);
}

static void env_restore(void) {
    eval_ruby("h = ($__aether_env || []).pop; ENV.replace(h) if h; nil");
}

int ruby_run(const char* code) {
    if (!code) return -1;
    if (ruby_init_host() != 0) return -1;
    return eval_ruby(code);
}

int ruby_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (ruby_init_host() != 0) return -1;
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;

    env_scrub(perms);
    aether_sandbox_push(perms);
    int result = eval_ruby(code);
    aether_sandbox_pop();
    env_restore();
    return result;
}

// --- Shared map for Ruby ------------------------------------------------------
//
// aether_map_get(key) -> String or nil, aether_map_put(key, value) -> nil, as
// in the Python bridge: real calls into the host's map, which refuses a write
// to a key the host gave as an input (frozen for the run). aether's copy kept
// a Ruby hash of the inputs and a second one for the outputs, which never
// reached the host's map.

static uint64_t rb_current_map_token = 0;
static int map_functions_defined = 0;

static VALUE rb_aether_map_get(VALUE self, VALUE key) {
    (void)self;
    const char* val = aether_shared_map_get_by_token(rb_current_map_token, string_ptr(key));
    return val ? g_rb.rb_utf8_str_new(val, (long)strlen(val)) : bridge_qnil;
}

static VALUE rb_aether_map_put(VALUE self, VALUE key, VALUE value) {
    (void)self;
    // string_ptr's results are only valid while their VALUEs are, so copy
    // the key before converting the value.
    char* k = strdup(string_ptr(key));
    if (!k) return bridge_qnil;
    aether_shared_map_put_by_token(rb_current_map_token, k, string_ptr(value));
    free(k);
    return bridge_qnil;
}

static void define_map_functions(void) {
    if (map_functions_defined) return;
    g_rb.rb_define_global_function("aether_map_get", (void*)rb_aether_map_get, 1);
    g_rb.rb_define_global_function("aether_map_put", (void*)rb_aether_map_put, 2);
    map_functions_defined = 1;
}

int ruby_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    if (ruby_init_host() != 0) return -1;
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;

    define_map_functions();
    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);
    uint64_t previous_map_token = rb_current_map_token;
    rb_current_map_token = map_token;

    env_scrub(perms);
    aether_sandbox_push(perms);
    int result = eval_ruby(code);
    aether_sandbox_pop();
    env_restore();

    rb_current_map_token = previous_map_token;
    return result;
}

// --- Tier 2: host functions and typed values ------------------------------------
//
// A host function is an Aether `@c_callback (ctx: ptr) -> int`, registered
// under a name and callable from Ruby as a method of that name. All of them
// are reached through one C method, __aether_dispatch(name, *args), and a
// `def name(*a)` that forwards to it, so the method needs no per-function C
// entry. The callback reads its arguments and pushes its return value
// through ctx (one call's frame); returning 0 returns nil.

#define MAX_HOST_FNS 256
static struct { char* name; int (*fn)(void*); } host_fns[MAX_HOST_FNS];
static int host_fn_count = 0;
static int dispatch_defined = 0;

typedef struct ae_rb_call {
    int argc;
    const VALUE* argv;
    VALUE ret;
    char* err;
    struct ae_rb_strs { char* s; struct ae_rb_strs* next; } *strs;   // copies handed out by arg_str
} ae_rb_call;

static const VALUE* arg_at(ae_rb_call* c, int i) {
    if (!c || i < 1 || i > c->argc) return NULL;
    return &c->argv[i - 1];
}

int ruby_arg_count(void* ctx) { return ctx ? ((ae_rb_call*)ctx)->argc : 0; }

int ruby_arg_type(void* ctx, int i) {
    const VALUE* a = arg_at((ae_rb_call*)ctx, i);
    if (!a) return AE_RB_TNIL;
    if (*a == bridge_qnil) return AE_RB_TNIL;
    if (*a == bridge_qtrue || *a == bridge_qfalse) return AE_RB_TBOOL;
    VALUE k = g_rb.rb_obj_class(*a);
    if (k == *g_rb.rb_cInteger) return AE_RB_TINT;
    if (k == *g_rb.rb_cString) return AE_RB_TSTR;
    if (k == *g_rb.rb_cFloat) return AE_RB_TFLOAT;
    return AE_RB_TOTHER;
}

// A string argument (to_s of other types; "" for nil), valid until the
// callback returns.
const char* ruby_arg_str(void* ctx, int i) {
    ae_rb_call* c = (ae_rb_call*)ctx;
    const VALUE* a = arg_at(c, i);
    if (!a || *a == bridge_qnil) return "";
    char* copy = strdup(string_ptr(*a));
    if (!copy) return "";
    struct ae_rb_strs* n = (struct ae_rb_strs*)malloc(sizeof *n);
    if (!n) { free(copy); return ""; }
    n->s = copy;
    n->next = c->strs;
    c->strs = n;
    return copy;
}

typedef struct { VALUE v; long long i; double d; } num_box;
static VALUE num2ll_thunk(VALUE p) { num_box* b = (num_box*)p; b->i = g_rb.rb_num2ll(b->v); return bridge_qnil; }
static VALUE num2dbl_thunk(VALUE p) { num_box* b = (num_box*)p; b->d = g_rb.rb_num2dbl(b->v); return bridge_qnil; }

// An integer argument; a float truncates, anything else (or an integer past
// 64 bits) reads 0. Never raises.
int64_t ruby_arg_int(void* ctx, int i) {
    const VALUE* a = arg_at((ae_rb_call*)ctx, i);
    int t = a ? ruby_arg_type(ctx, i) : AE_RB_TNIL;
    if (t != AE_RB_TINT && t != AE_RB_TFLOAT) return 0;
    num_box b = { *a, 0, 0 };
    int st = 0;
    g_rb.rb_protect(num2ll_thunk, (VALUE)&b, &st);
    if (st) { g_rb.rb_set_errinfo(bridge_qnil); return 0; }
    return (int64_t)b.i;
}

double ruby_arg_double(void* ctx, int i) {
    const VALUE* a = arg_at((ae_rb_call*)ctx, i);
    int t = a ? ruby_arg_type(ctx, i) : AE_RB_TNIL;
    if (t != AE_RB_TINT && t != AE_RB_TFLOAT) return 0;
    num_box b = { *a, 0, 0 };
    int st = 0;
    g_rb.rb_protect(num2dbl_thunk, (VALUE)&b, &st);
    if (st) { g_rb.rb_set_errinfo(bridge_qnil); return 0; }
    return b.d;
}

// 1 only for `true`.
int ruby_arg_bool(void* ctx, int i) {
    const VALUE* a = arg_at((ae_rb_call*)ctx, i);
    return a && *a == bridge_qtrue;
}

void ruby_push_int(void* ctx, int64_t v) { if (ctx) ((ae_rb_call*)ctx)->ret = g_rb.rb_ll2inum(v); }
void ruby_push_double(void* ctx, double v) { if (ctx) ((ae_rb_call*)ctx)->ret = g_rb.rb_float_new(v); }
void ruby_push_bool(void* ctx, int v) { if (ctx) ((ae_rb_call*)ctx)->ret = v ? bridge_qtrue : bridge_qfalse; }
void ruby_push_nil(void* ctx) { if (ctx) ((ae_rb_call*)ctx)->ret = bridge_qnil; }
void ruby_push_str(void* ctx, const char* v) {
    if (ctx) ((ae_rb_call*)ctx)->ret = g_rb.rb_utf8_str_new(v ? v : "", (long)strlen(v ? v : ""));
}

// `return ruby.raise_error(ctx, msg)`: the script sees RuntimeError(msg)
// after the callback returns, so its defers have run.
int ruby_raise_error(void* ctx, const char* msg) {
    ae_rb_call* c = (ae_rb_call*)ctx;
    if (c) {
        free(c->err);
        c->err = strdup(msg ? msg : "error");
    }
    return 0;
}

static VALUE rb_aether_dispatch(int argc, VALUE* argv, VALUE self) {
    (void)self;
    ae_rb_call c = { argc > 0 ? argc - 1 : 0, argv + 1, bridge_qnil, NULL, NULL };
    int (*fn)(void*) = NULL;
    if (argc > 0) {
        volatile VALUE nm = argv[0];
        const char* name = g_rb.rb_string_value_ptr(&nm);
        for (int i = 0; i < host_fn_count; i++) {
            if (strcmp(host_fns[i].name, name) == 0) { fn = host_fns[i].fn; break; }
        }
    }
    if (!fn) c.err = strdup("no such host function");
    else if (fn(&c) == 0 && !c.err) c.ret = bridge_qnil;
    for (struct ae_rb_strs* s = c.strs; s;) {
        struct ae_rb_strs* next = s->next;
        free(s->s);
        free(s);
        s = next;
    }
    if (c.err) {
        VALUE e = g_rb.rb_exc_new(*g_rb.rb_eRuntimeError, c.err, (long)strlen(c.err));
        free(c.err);
        g_rb.rb_exc_raise(e);
    }
    return c.ret;
}

// Registers `fn` as the Ruby method `name` (a plain method name). 0 on
// success, -1 on a bad name, a full table or no interpreter.
int ruby_register(const char* name, void* host_fn) {
    if (!name || !host_fn || ruby_init_host() != 0) return -1;
    if (!*name) return -1;
    for (const char* p = name; *p; p++) {
        int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_' ||
                 (p != name && *p >= '0' && *p <= '9');
        if (!ok) return -1;
    }
    int slot = -1;
    for (int i = 0; i < host_fn_count; i++) {
        if (strcmp(host_fns[i].name, name) == 0) slot = i;
    }
    if (slot < 0) {
        if (host_fn_count >= MAX_HOST_FNS) return -1;
        slot = host_fn_count++;
        host_fns[slot].name = strdup(name);
    }
    host_fns[slot].fn = (int (*)(void*))host_fn;
    if (!dispatch_defined) {
        g_rb.rb_define_global_function("__aether_dispatch", (void*)rb_aether_dispatch, -1);
        dispatch_defined = 1;
    }
    char def[512];
    snprintf(def, sizeof def, "def %s(*a); __aether_dispatch('%s', *a); end; nil", name, name);
    return eval_ruby(def);
}

static char* g_result = NULL;

// --- the instruction limit ---------------------------------------------------
//
// A TracePoint hook on line, method call, C call and block call events,
// installed for the outermost ruby_eval and counting them. Once the budget is spent it raises
// RuntimeError("instruction limit exceeded") on every later event, so a
// script that rescues it is stopped again at its handler's next line. It
// bounds Ruby code only: a loop inside C that makes no call (`Array.new(10**9)`),
// a loop that makes no event of any kind (`while true; end`, `x += 1` on one
// line: Integer#+ is an instruction, not a call, and a line is counted when the
// line changes) and `sleep` are not stopped, and a script can
// `ObjectSpace.each_object(TracePoint)` and disable it. The contained child
// is the answer to those.

#define AE_RB_EVENT_LINE   0x0001u
#define AE_RB_EVENT_CALL   0x0008u
#define AE_RB_EVENT_C_CALL 0x0020u
#define AE_RB_EVENT_B_CALL 0x0100u
static long limit_budget = 0;
static long limit_count = 0;
static int limit_depth = 0;
static VALUE limit_tp = 0;      // registered with the GC while it is set
static int limit_tp_registered = 0;

static void limit_hook(VALUE tp, void* data) {
    (void)tp; (void)data;
    if (++limit_count > limit_budget) {
        const char* m = "instruction limit exceeded";
        g_rb.rb_exc_raise(g_rb.rb_exc_new(*g_rb.rb_eRuntimeError, m, (long)strlen(m)));
    }
}

// Bounds each outermost ruby_eval to `budget` line and call events; 0
// disarms. "" when set.
const char* ruby_set_instruction_limit(int64_t budget) {
    if (ruby_init_host() != 0) return "ruby is not available";
    limit_budget = budget > 0 ? (long)budget : 0;
    return "";
}

static void limit_enter(void) {
    if (limit_budget <= 0 || limit_depth++ > 0) return;
    limit_count = 0;
    if (!limit_tp_registered) {
        limit_tp = bridge_qnil;
        g_rb.rb_gc_register_address(&limit_tp);
        limit_tp_registered = 1;
    }
    limit_tp = g_rb.rb_tracepoint_new(bridge_qnil, AE_RB_EVENT_LINE | AE_RB_EVENT_CALL | AE_RB_EVENT_C_CALL | AE_RB_EVENT_B_CALL, limit_hook, NULL);
    g_rb.rb_tracepoint_enable(limit_tp);
}

static void limit_leave(int entered) {
    if (!entered) return;
    if (--limit_depth > 0) return;
    g_rb.rb_tracepoint_disable(limit_tp);
    limit_tp = bridge_qnil;
}

// Evaluates `code` and keeps the result: the value's to_s on success, or
// "<Class>: <message>" on an exception. 0, or -1 on an exception. Unlike
// ruby_run, prints nothing.
int ruby_eval(const char* code) {
    free(g_result);
    g_result = NULL;
    if (!code || ruby_init_host() != 0) { g_result = strdup("Ruby is not available"); return -1; }
    int state = 0;
    int entered = limit_budget > 0;
    limit_enter();
    volatile VALUE v = g_rb.rb_eval_string_protect(code, &state);
    limit_leave(entered);
    if (state) {
        g_result = take_error(1);
        if (!g_result) g_result = strdup("error");
        return -1;
    }
    g_result = strdup(v == bridge_qnil ? "" : string_ptr(v));
    return 0;
}

const char* ruby_result_str(void) { return g_result ? g_result : ""; }

#else
#include <stdio.h>
int ruby_init_host(void) {
    fprintf(stderr, "error: ruby not available (compile with AETHER_HAS_RUBY)\n");
    return -1;
}
void ruby_finalize_host(void) {}
int ruby_run(const char* code) { (void)code; return ruby_init_host(); }
int ruby_run_sandboxed(void* perms, const char* code) {
    (void)perms; (void)code;
    return ruby_init_host();
}
int ruby_run_sandboxed_with_map(void* perms, const char* code, uint64_t token) {
    (void)perms; (void)code; (void)token;
    return ruby_init_host();
}
#endif
