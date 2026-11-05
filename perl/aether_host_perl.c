// aether_host_perl.c — Embedded Perl Language Host Module
//
// Embeds Perl in the Aether process. Perl's open(), $ENV{},
// system() etc. go through libc and are intercepted by the
// LD_PRELOAD sandbox layer.
//
// LOADING MODEL: dlopen, not -lperl. Mirrors contrib/host/ruby,
// contrib/host/tcl — every Perl C-API symbol
// resolved via dlsym at first call. The bridge has NO unresolved
// Perl symbols at link time, so end-user binaries have no
// DT_NEEDED libperl and are ABI-portable across deploy-host Perl
// versions (5.34 / 5.36 / 5.38 / …). The Perl types it needs are declared
// in perl_abi.h: no perl dev headers are needed to build it either.
//
// The Perl bridge is the most macro-heavy of the four:
//
//   eval_pv(s, croak)       → Perl_eval_pv(my_perl, s, croak)
//   ERRSV                   → Perl_get_sv(my_perl, "@", 0)
//   SvTRUE(sv)              → Perl_sv_true(my_perl, sv)
//   SvPV_nolen(sv)          → Perl_sv_2pv_flags(my_perl, sv, NULL, SV_GMAGIC)
//
// All the macros expand to context-passing (pTHX_) wrappers around
// real Perl_* exported functions. We dlsym those Perl_* functions
// directly and pass `my_perl` (the per-interpreter context) as the
// first argument explicitly. SV_GMAGIC's value (= 2) is a stable
// public constant baked into the bridge — same shape as Ruby's
// Qnil-as-literal.
//
// What we DON'T do: dlsym SV-struct-field accessors (SvFLAGS,
// SvPVX, …). Those would bake the SV layout into the bridge,
// which differs across Perl versions. Calling Perl_* functions
// instead is the layout-agnostic path.
//
// PERL CALLING THE HOST. An XSUB reads its arguments from the interpreter's
// stack (dXSARGS), which is interpreter layout. Instead one scalar,
// $main::_aether_call, carries a get hook (a MGVTBL, perlguts "Magic Virtual
// Tables"): a Perl sub puts a request, length-prefixed, in
// $main::_aether_req and reads $main::_aether_call; the hook decodes the
// request, does it in C and sets the scalar to the answer, which the sub
// decodes. The prelude below is the Perl half. It carries the host functions
// perl.register makes (each a Perl sub that asks for the Aether callback by
// name), aether_map_get and aether_map_put (the shared map, by token), and
// the check of an environment variable against the grants in force.

#include "aether_host_perl.h"
// The installed runtime's headers (include/aether/runtime, include/aether/std),
// which `ae build` puts on the include path of every C file it compiles into
// a program. libaether is always built with the sandbox; aether's Makefile
// says so to its own C with -DAETHER_HAS_SANDBOX, which a program's build
// does not pass.
#ifndef AETHER_HAS_SANDBOX
#define AETHER_HAS_SANDBOX 1
#endif
#include "aether_sandbox.h"
#include "aether_shared_map.h"

// In aether's tree this was defined by `make contrib` when the perl dev
// headers were found. Here the declarations travel with the bridge
// (perl_abi.h), so it always builds; libperl is still found only at run time.
#ifndef AETHER_HAS_PERL
#define AETHER_HAS_PERL 1
#endif

#if AETHER_HAS_PERL
#include "perl_abi.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char** environ;

// --- libperl dlopen table ---------------------------------------------------

static void* libperl_handle = NULL;

static struct {
    // Lifecycle (un-prefixed, real exports).
    PerlInterpreter* (*perl_alloc)(void);
    void  (*perl_construct)(PerlInterpreter*);
    int   (*perl_parse)(PerlInterpreter*, XSINIT_t xsinit, int argc, char** argv, char** env);
    int   (*perl_run)(PerlInterpreter*);
    int   (*perl_destruct)(PerlInterpreter*);
    void  (*perl_free)(PerlInterpreter*);
    // Eval + error introspection (Perl_*-prefixed; we pass my_perl
    // as first arg explicitly instead of via pTHX_ macro).
    SV*   (*Perl_eval_pv)(PerlInterpreter*, const char*, I32 croak_on_error);
    SV*   (*Perl_get_sv)(PerlInterpreter*, const char* name, I32 flags);
    I32   (*Perl_sv_true)(PerlInterpreter*, SV*);
    char* (*Perl_sv_2pv_flags)(PerlInterpreter*, SV*, STRLEN* lp, U32 flags);
    // Perl calling the host (see above).
    MAGIC* (*Perl_sv_magicext)(PerlInterpreter*, SV* sv, SV* obj, int how,
                               const MGVTBL* vtbl, const char* name, I32 namlen);
    void  (*Perl_sv_setpvn)(PerlInterpreter*, SV*, const char*, STRLEN);
    // DynaLoader's native half, which a perl_parse with no xsinit would
    // leave out (so `use Socket`, `use POSIX` and every other XS module
    // would fail to load).
    CV*   (*Perl_newXS)(PerlInterpreter*, const char* name, XSUBADDR_t, const char* file);
    // PERL_SYS_INIT3, which is a function from 5.34 on (a macro before);
    // optional.
    void  (*Perl_sys_init3)(int* argc, char*** argv, char*** env);
    // Optional: threaded builds only.
    void  (*Perl_set_context)(void*);
} g_pl;

static int resolve_perl_symbols(void* h) {
#define RESOLVE(field, sym) do {                                       \
        *(void**)(&g_pl.field) = dlsym(h, sym);                        \
        if (!g_pl.field) {                                             \
            fprintf(stderr,                                            \
                "aether host_perl: libperl missing symbol %s\n", sym); \
            return -1;                                                 \
        }                                                              \
    } while (0)

    RESOLVE(perl_alloc,        "perl_alloc");
    RESOLVE(perl_construct,    "perl_construct");
    RESOLVE(perl_parse,        "perl_parse");
    RESOLVE(perl_run,          "perl_run");
    RESOLVE(perl_destruct,     "perl_destruct");
    RESOLVE(perl_free,         "perl_free");
    RESOLVE(Perl_eval_pv,      "Perl_eval_pv");
    RESOLVE(Perl_get_sv,       "Perl_get_sv");
    RESOLVE(Perl_sv_true,      "Perl_sv_true");
    RESOLVE(Perl_sv_2pv_flags, "Perl_sv_2pv_flags");
    RESOLVE(Perl_sv_magicext,  "Perl_sv_magicext");
    RESOLVE(Perl_sv_setpvn,    "Perl_sv_setpvn");
    RESOLVE(Perl_newXS,        "Perl_newXS");
    *(void**)(&g_pl.Perl_set_context) = dlsym(h, "Perl_set_context");
    *(void**)(&g_pl.Perl_sys_init3) = dlsym(h, "Perl_sys_init3");
    return 0;
#undef RESOLVE
}

static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

// Load libperl on first use. Strict two-step contract:
//   1. ${AETHER_PERL_SONAME} env var (orchestrator-supplied exact soname or
//      path, e.g. "libperl.so.5.36"). The orchestrator probes with
//      `perl -MConfig -e 'print "$Config{archlibexp}/CORE/$Config{libperl}"'`
//      (tests/perl_env.sh) and passes that.
//   2. libperl.so (Debian-style flat symlink, when present).
// No hardcoded version-list fallback. Orchestrator owns the probe.
static int load_libperl(void) {
    if (libperl_handle) return 0;

    const char* env_soname = getenv("AETHER_PERL_SONAME");
    void* h = try_dlopen(env_soname);
    if (!h) h = try_dlopen("libperl.so");
    if (!h) {
        // dlerror() clears the error it reports: read it once.
        const char* why = dlerror();
        fprintf(stderr,
            "aether host_perl: cannot dlopen libperl "
            "(tried $AETHER_PERL_SONAME=%s, libperl.so).\n"
            "  Install a perl 5.x runtime on the host, or set "
            "AETHER_PERL_SONAME to the exact soname or path.\n"
            "  Hint: $(perl -MConfig -e 'print \"$Config{archlibexp}/CORE/$Config{libperl}\"')\n"
            "  dlerror: %s\n",
            env_soname ? env_soname : "(unset)",
            why ? why : "(none)");
        return -1;
    }

    // A perl built without MULTIPLICITY has no interpreter argument in its
    // API and exports the interpreter's variables as globals; a
    // MULTIPLICITY build keeps them in the interpreter. perl_abi.h says why
    // this is refused.
    if (dlsym(h, "PL_errgv")) {
        fprintf(stderr,
            "aether host_perl: %s is a perl built without MULTIPLICITY "
            "(its API takes no interpreter argument), which this bridge "
            "does not support\n",
            env_soname && *env_soname ? env_soname : "libperl.so");
        dlclose(h);
        return -1;
    }

    if (resolve_perl_symbols(h) != 0) {
        dlclose(h);
        return -1;
    }
    libperl_handle = h;
    return 0;
}

// --- bridge state ------------------------------------------------------------

static PerlInterpreter* my_perl = NULL;

// One interpreter, one thread in it at a time. Recursive: a host function
// may call back into the bridge.
static pthread_mutex_t g_lock;
static pthread_once_t g_lock_once = PTHREAD_ONCE_INIT;
static void lock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lock, &a);
    pthread_mutexattr_destroy(&a);
}
static void enter(void) {
    pthread_once(&g_lock_once, lock_init);
    pthread_mutex_lock(&g_lock);
    // A threaded perl's current interpreter is per thread.
    if (my_perl && g_pl.Perl_set_context) g_pl.Perl_set_context(my_perl);
}
static void leave(void) { pthread_mutex_unlock(&g_lock); }

static uint64_t g_map_token = 0;

// --- host functions ----------------------------------------------------------

typedef struct {
    char* name;
    int (*fn)(void* ctx);
} HostFn;
static HostFn* g_fns = NULL;
static int g_nfns = 0;

// The state of one call from Perl to a registered Aether callback.
typedef struct {
    int argc;
    char** argv;        // decoded, NUL-terminated copies
    size_t* argl;
    char* result;       // the last push, or NULL
    char* error;        // raise_error's message, or NULL
} PerlCall;

#define AE_PERL_RAISE (-2)

// A request is a list of strings, each "<byte length>:<bytes>". Returns the
// items (pointers into a malloc'd copy that *buf owns) or -1.
static int decode_items(const char* s, size_t n, char** buf, char*** items, size_t** lens) {
    char* copy = malloc(n + 1);
    if (!copy) return -1;
    memcpy(copy, s, n);
    copy[n] = 0;
    int cap = 8, cnt = 0;
    char** it = malloc(sizeof(char*) * cap);
    size_t* ln = malloc(sizeof(size_t) * cap);
    size_t i = 0;
    while (i < n) {
        size_t len = 0;
        int digits = 0;
        while (i < n && copy[i] >= '0' && copy[i] <= '9') { len = len * 10 + (size_t)(copy[i] - '0'); i++; digits++; }
        if (!digits || i >= n || copy[i] != ':' || len > n - (i + 1)) { free(copy); free(it); free(ln); return -1; }
        i++;
        if (cnt == cap) { cap *= 2; it = realloc(it, sizeof(char*) * cap); ln = realloc(ln, sizeof(size_t) * cap); }
        it[cnt] = copy + i;
        ln[cnt] = len;
        cnt++;
        i += len;
    }
    *buf = copy; *items = it; *lens = ln;
    return cnt;
}

// An item is not NUL terminated in the buffer (the next item's length digits
// follow it): the ones used as C strings are copied out.
static char* dup_item(const char* p, size_t len) {
    char* c = malloc(len + 1);
    if (!c) return NULL;
    memcpy(c, p, len);
    c[len] = 0;
    return c;
}

// The answer is "O<bytes>" (a value), "N" (undef) or "E<message>" (die).
static void set_answer(PerlInterpreter* p, SV* sv, char kind, const char* bytes, size_t len) {
    char* out = malloc(len + 1);
    if (!out) return;
    out[0] = kind;
    if (len) memcpy(out + 1, bytes, len);
    g_pl.Perl_sv_setpvn(p, sv, out, len + 1);
    free(out);
}

static char* pack_item_list(char** items, size_t* lens, int n, size_t* outlen) {
    size_t total = 0;
    for (int i = 0; i < n; i++) total += lens[i] + 24;
    char* out = malloc(total + 1);
    size_t pos = 0;
    for (int i = 0; i < n; i++) {
        pos += (size_t)snprintf(out + pos, total + 1 - pos, "%zu:", lens[i]);
        memcpy(out + pos, items[i], lens[i]);
        pos += lens[i];
    }
    *outlen = pos;
    return out;
}

static int env_allowed(const char* name) {
    return aether_sandbox_check_slow("env", name) != 0;
}

// $main::_aether_call is read: do what $main::_aether_req asks.
static int call_get(PerlInterpreter* p, SV* sv, MAGIC* mg) {
    (void)mg;
    SV* req = g_pl.Perl_get_sv(p, "main::_aether_req", 0);
    STRLEN rlen = 0;
    const char* rbytes = req ? g_pl.Perl_sv_2pv_flags(p, req, &rlen, SV_GMAGIC) : NULL;
    char* buf = NULL; char** it = NULL; size_t* ln = NULL;
    int n = rbytes ? decode_items(rbytes, rlen, &buf, &it, &ln) : -1;
    if (n < 1) {
        set_answer(p, sv, 'E', "aether host_perl: malformed request", 35);
        free(buf); free(it); free(ln);
        return 0;
    }
    char op = ln[0] ? it[0][0] : '?';
    // Item strings are not NUL terminated in the buffer: copy those used as C strings.
    char* a1 = n > 1 ? dup_item(it[1], ln[1]) : NULL;
    char* a2 = n > 2 ? dup_item(it[2], ln[2]) : NULL;
    switch (op) {
    case 'G': {   // map get: key
        const char* v = (a1 && g_map_token) ? aether_shared_map_get_by_token(g_map_token, a1) : NULL;
        if (v) set_answer(p, sv, 'O', v, strlen(v)); else set_answer(p, sv, 'N', "", 0);
        break;
    }
    case 'P': {   // map put: key value; answers 1 written, 0 refused
        int ok = (a1 && a2 && g_map_token) ? aether_shared_map_put_by_token(g_map_token, a1, a2) == 0 : 0;
        set_answer(p, sv, 'O', ok ? "1" : "0", 1);
        break;
    }
    case 'K': {   // map keys
        int cnt = g_map_token ? aether_shared_map_count_by_token(g_map_token) : 0;
        char** ks = calloc((size_t)cnt + 1, sizeof(char*));
        size_t* kl = calloc((size_t)cnt + 1, sizeof(size_t));
        int m = 0;
        for (int i = 0; i < cnt; i++) {
            const char* k = aether_shared_map_key_at_by_token(g_map_token, i);
            if (k) { ks[m] = (char*)k; kl[m] = strlen(k); m++; }
        }
        size_t ol = 0;
        char* o = pack_item_list(ks, kl, m, &ol);
        set_answer(p, sv, 'O', o, ol);
        free(o); free(ks); free(kl);
        break;
    }
    case 'V': {   // is an environment variable granted: name; answers 1 or 0
        set_answer(p, sv, 'O', (a1 && env_allowed(a1)) ? "1" : "0", 1);
        break;
    }
    case 'H': {   // host function: name args...
        int (*fn)(void*) = NULL;
        for (int i = 0; i < g_nfns && a1; i++) if (strcmp(g_fns[i].name, a1) == 0) { fn = g_fns[i].fn; break; }
        if (!fn) {
            char msg[256];
            int ml = snprintf(msg, sizeof(msg), "no host function %s", a1 ? a1 : "?");
            set_answer(p, sv, 'E', msg, (size_t)ml);
            break;
        }
        PerlCall c;
        c.argc = n - 2;
        c.argv = calloc((size_t)(c.argc > 0 ? c.argc : 1), sizeof(char*));
        c.argl = calloc((size_t)(c.argc > 0 ? c.argc : 1), sizeof(size_t));
        for (int i = 0; i < c.argc; i++) { c.argv[i] = dup_item(it[i + 2], ln[i + 2]); c.argl[i] = ln[i + 2]; }
        c.result = NULL; c.error = NULL;
        int rc = fn(&c);
        if (rc == AE_PERL_RAISE || c.error) {
            const char* e = c.error ? c.error : "error";
            set_answer(p, sv, 'E', e, strlen(e));
        } else if (rc == 1 && c.result) {
            set_answer(p, sv, 'O', c.result, strlen(c.result));
        } else {
            set_answer(p, sv, 'N', "", 0);
        }
        for (int i = 0; i < c.argc; i++) free(c.argv[i]);
        free(c.argv); free(c.argl); free(c.result); free(c.error);
        break;
    }
    default:
        set_answer(p, sv, 'E', "aether host_perl: unknown request", 33);
    }
    free(a1); free(a2); free(buf); free(it); free(ln);
    return 0;
}

static const MGVTBL g_call_vtbl = { call_get, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

// The Perl half of the above. Everything here is in package main under names
// that start _ae_, except the two map functions a script calls.
static const char PRELUDE[] =
    "package main;\n"
    "sub _ae_pack { join '', map { my $s = defined $_ ? \"$_\" : ''; utf8::encode($s); length($s) . ':' . $s } @_ }\n"
    "sub _ae_unpack { my $s = shift; my @o; while (length $s) { $s =~ s/^(\\d+):// or last; push @o, substr($s, 0, $1, ''); } @o }\n"
    "sub _ae_call {\n"
    "    $main::_aether_req = _ae_pack(@_);\n"
    "    my $r = $main::_aether_call;\n"
    "    my $k = substr($r, 0, 1); my $p = substr($r, 1);\n"
    "    utf8::decode($p);\n"
    "    die \"$p\\n\" if $k eq 'E';\n"
    "    return undef if $k eq 'N';\n"
    "    return $p;\n"
    "}\n"
    "sub aether_map_get { _ae_call('G', $_[0]) }\n"
    "sub aether_map_put { my $ok = _ae_call('P', $_[0], $_[1]); $main::_aether_output{$_[0]} = $_[1] if $ok; $ok }\n"
    "1;\n";

static void xs_init(PerlInterpreter* p) {
    void* boot = dlsym(libperl_handle, "boot_DynaLoader");
    if (boot) g_pl.Perl_newXS(p, "DynaLoader::boot_DynaLoader", (XSUBADDR_t)boot, "aether_host_perl.c");
}

static int run_perl_code(const char* code);

int aether_perl_init(void) {
    enter();
    if (my_perl) { leave(); return 0; }
    if (load_libperl() != 0) { leave(); return -1; }
    // PERL_SYS_INIT3, once per process: perl's global state (locale and
    // hash-seed set-up, mutexes). Without it macOS's perl_parse fails
    // (status 9) and a sub defined afterwards crashes the process. Never
    // undone with PERL_SYS_TERM: finalize then init again is supported, and
    // that is meant for the end of a process.
    static int sys_inited = 0;
    if (!sys_inited && g_pl.Perl_sys_init3) {
        static char* sys_argv[] = { "", "-e", "0", NULL };
        int sys_argc = 3;
        char** sys_av = sys_argv;
        char** sys_env = environ;
        g_pl.Perl_sys_init3(&sys_argc, &sys_av, &sys_env);
        sys_inited = 1;
    }
    PerlInterpreter* p = g_pl.perl_alloc();
    if (!p) { leave(); return -1; }
    my_perl = p;
    if (g_pl.Perl_set_context) g_pl.Perl_set_context(p);
    g_pl.perl_construct(p);

    char* embedding[] = { "", "-e", "0" };
    int parsed = g_pl.perl_parse(p, xs_init, 3, embedding, NULL);
    if (parsed != 0) {
        fprintf(stderr, "aether host_perl: perl_parse failed (status %d)\n", parsed);
        g_pl.perl_destruct(p);
        g_pl.perl_free(p);
        my_perl = NULL;
        leave();
        return -1;
    }
    g_pl.perl_run(p);

    // The host's half of the prelude: the get hook on $main::_aether_call.
    SV* callsv = g_pl.Perl_get_sv(p, "main::_aether_call", GV_ADD);
    if (callsv) g_pl.Perl_sv_magicext(p, callsv, NULL, PERL_MAGIC_ext, &g_call_vtbl, NULL, 0);
    if (run_perl_code(PRELUDE) != 0) {
        leave();
        return -1;
    }
    leave();
    return 0;
}

void aether_perl_finalize(void) {
    enter();
    if (my_perl) {
        g_pl.perl_destruct(my_perl);
        g_pl.perl_free(my_perl);
        my_perl = NULL;
    }
    leave();
}

// Safe eval — catches exceptions and prints them.
//
// Replaces the original `eval_pv` + `SvTRUE(ERRSV)` + `SvPV_nolen(ERRSV)`
// macros with explicit Perl_* calls passing my_perl.
//   ERRSV          → Perl_get_sv(my_perl, "@", 0)  (returns $@ SV)
//   SvTRUE(sv)     → Perl_sv_true(my_perl, sv)
//   SvPV_nolen(sv) → Perl_sv_2pv_flags(my_perl, sv, NULL, SV_GMAGIC)
static int run_perl_code(const char* code) {
    (void)g_pl.Perl_eval_pv(my_perl, code, 0 /* croak_on_error=FALSE */);
    SV* errsv = g_pl.Perl_get_sv(my_perl, "@", 0);
    if (errsv && g_pl.Perl_sv_true(my_perl, errsv)) {
        char* msg = g_pl.Perl_sv_2pv_flags(my_perl, errsv, NULL, SV_GMAGIC);
        fprintf(stderr, "[perl] %s\n", msg ? msg : "(no message)");
        return -1;
    }
    return 0;
}

int aether_perl_run(const char* code) {
    if (!code) return -1;
    if (aether_perl_init() != 0) return -1;
    enter();
    int rc = run_perl_code(code);
    leave();
    return rc;
}

// Perl keeps %ENV as a copy taken at start, before any sandbox, so no libc
// interposer sees a script read it. Hide what the grants in force do not
// grant for the length of the run: the variables are deleted (which also
// unsets them in the process, Perl's %ENV being tied to environ) and put
// back after, so the host's own environment is whole again afterwards.
static const char ENV_HIDE[] =
    "@main::_ae_saved_env = %ENV;\n"
    "for my $k (keys %ENV) { delete $ENV{$k} unless _ae_call('V', $k) eq '1' }\n";
static const char ENV_RESTORE[] =
    "%ENV = @main::_ae_saved_env; @main::_ae_saved_env = ();\n";

static int sandboxed_run(void* perms, const char* code) {
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;
    aether_sandbox_push(perms);
    run_perl_code(ENV_HIDE);
    int result = run_perl_code(code);
    run_perl_code(ENV_RESTORE);
    aether_sandbox_pop();
    return result;
}

int aether_perl_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (aether_perl_init() != 0) return -1;
    enter();
    int result = sandboxed_run(perms, code);
    leave();
    return result;
}

// --- Shared map for Perl ---------------------------------------------------

int aether_perl_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    if (aether_perl_init() != 0) return -1;

    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);

    enter();
    uint64_t previous_map_token = g_map_token;
    g_map_token = map_token;

    // The inputs as %_aether_input, read through the map (never spliced into
    // source), and aether_map_get / aether_map_put on the map itself; what
    // a script puts is also in %_aether_output.
    run_perl_code(
        "our %_aether_input = (); our %_aether_output = ();\n"
        "$_aether_input{$_} = aether_map_get($_) for _ae_unpack(_ae_call('K'));\n");

    int result = sandboxed_run(perms, code);

    g_map_token = previous_map_token;
    leave();
    return result;
}

// --- Host functions the script calls (perl.register) ---------------------------

// Defines the Perl sub `name`; calling it asks the callback `fn`. Its
// arguments are strings (perl.arg_str / arg_int), its answer one string
// (perl.push_str / push_int), or undef when nothing is pushed.
int aether_perl_register(const char* name, void* fn) {
    if (!name || !*name || !fn) return -1;
    for (const char* c = name; *c; c++) {
        int ok = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || *c == '_' || *c == ':' ||
                 (c != name && *c >= '0' && *c <= '9');
        if (!ok) return -1;
    }
    if (aether_perl_init() != 0) return -1;
    enter();
    int slot = -1;
    for (int i = 0; i < g_nfns; i++) if (strcmp(g_fns[i].name, name) == 0) { slot = i; break; }
    if (slot < 0) {
        g_fns = realloc(g_fns, sizeof(HostFn) * (size_t)(g_nfns + 1));
        slot = g_nfns++;
        g_fns[slot].name = strdup(name);
    }
    g_fns[slot].fn = (int (*)(void*))fn;
    char code[512];
    snprintf(code, sizeof(code), "no warnings 'redefine'; sub %s { _ae_call('H', '%s', @_) } 1;", name, name);
    int rc = run_perl_code(code);
    leave();
    return rc;
}

int aether_perl_arg_count(void* ctx) { return ctx ? ((PerlCall*)ctx)->argc : 0; }

const char* aether_perl_arg_str(void* ctx, int i) {
    PerlCall* c = (PerlCall*)ctx;
    if (!c || i < 1 || i > c->argc) return "";
    return c->argv[i - 1];
}

int64_t aether_perl_arg_int(void* ctx, int i) {
    return strtoll(aether_perl_arg_str(ctx, i), NULL, 10);
}

void aether_perl_push_str(void* ctx, const char* v) {
    PerlCall* c = (PerlCall*)ctx;
    if (!c) return;
    free(c->result);
    c->result = strdup(v ? v : "");
}

void aether_perl_push_int(void* ctx, int64_t v) {
    char b[32];
    snprintf(b, sizeof(b), "%lld", (long long)v);
    aether_perl_push_str(ctx, b);
}

// `return perl.raise_error(ctx, msg)`: the Perl sub dies with msg.
int aether_perl_raise_error(void* ctx, const char* msg) {
    PerlCall* c = (PerlCall*)ctx;
    if (!c) return AE_PERL_RAISE;
    free(c->error);
    c->error = strdup(msg ? msg : "error");
    return AE_PERL_RAISE;
}

#else
#include <stdio.h>
int aether_perl_init(void) {
    fprintf(stderr, "error: perl not available (compiled with AETHER_HAS_PERL=0)\n");
    return -1;
}
void aether_perl_finalize(void) {}
int aether_perl_run(const char* code) { (void)code; return aether_perl_init(); }
int aether_perl_run_sandboxed(void* perms,
    const char* code) {
  (void)perms; (void)code;
  return aether_perl_init();
}
int aether_perl_run_sandboxed_with_map(void* perms, const char* code,
    uint64_t token) {
  (void)perms; (void)code; (void)token;
  return aether_perl_init();
}
int aether_perl_register(const char* name, void* fn) { (void)name; (void)fn; return aether_perl_init(); }
int aether_perl_arg_count(void* ctx) { (void)ctx; return 0; }
const char* aether_perl_arg_str(void* ctx, int i) { (void)ctx; (void)i; return ""; }
int64_t aether_perl_arg_int(void* ctx, int i) { (void)ctx; (void)i; return 0; }
void aether_perl_push_str(void* ctx, const char* v) { (void)ctx; (void)v; }
void aether_perl_push_int(void* ctx, int64_t v) { (void)ctx; (void)v; }
int aether_perl_raise_error(void* ctx, const char* msg) { (void)ctx; (void)msg; return -1; }
#endif
