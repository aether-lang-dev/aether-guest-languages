// aether_host_racket.c: Embedded Racket / Rhombus language host.
//
// Embeds Racket CS in the Aether process: boots it once, then evaluates source
// text and hands the captured output back into Aether. Racket and Rhombus are
// the same VM (Rhombus is a #lang on the Racket runtime), so one persistent VM
// backs both `import racket` and `import rhombus`. State survives across
// calls, so set / evaluate / get share a namespace.
//
// LOADING MODEL: dlopen, as for the other languages here. Racket CS's
// embedding API (racket_boot, racket_eval, ...) is a library only where
// Racket is built as one: the macOS distribution ships it as
// Racket.framework, a dylib. Upstream's configure refuses --enable-shared on
// every other platform, so elsewhere the library is libracketcs.so made from
// the distribution's libracketcs.a (tests/racket_env.sh and racket/README.md
// say how). Every symbol is resolved with dlsym at the first call; the bridge
// has no unresolved Racket symbol at link time, and the Racket headers are
// not needed to build a program that imports it (racket_abi.h carries what it
// uses). In aether's copy the bridge was linked against libracketcs.a at
// build time instead.
//
// VALUE MODEL: Racket CS values are Chez Scheme objects. The bridge never
// builds a large datum by hand: it reads its own Racket prelude (below) with
// a small s-expression reader, evaluates that once, and afterwards calls the
// prelude's procedures with a code string. Results come back as a UTF-8
// bytevector. Objects built in C stay valid because nothing here runs Scheme
// between allocating one and handing it to racket_eval (Chez collects only
// when Scheme runs).
//
// THREADS: Racket CS runs on the thread that booted it. Call the bridge from
// one thread (the main thread), not from several actors.

// dladdr (to find the files beside the library) is a GNU extension on Linux.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aether_host_racket.h"
#ifndef AETHER_HAS_SANDBOX
#define AETHER_HAS_SANDBOX 1
#endif
#include "aether_sandbox.h"
#include "aether_shared_map.h"
#include "aether_string.h"

#include "racket_abi.h"

#include <dirent.h>
#include <dlfcn.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// --- the library --------------------------------------------------------------

static void* g_lib = NULL;
static int g_new_boot = 0;          // racket_boot_arguments_new_t (Racket 8.14 and later)

static struct {
    void (*racket_boot)(void* args);
    ptr (*racket_eval)(ptr s_expr);
    void (*racket_namespace_require)(ptr module_path);
    ptr (*Sstring_to_symbol)(const char*);
    ptr (*Scons)(ptr, ptr);
    ptr (*Sstring_utf8)(const char*, iptr);
    ptr (*Sinteger)(iptr);
} R;

static int resolve_symbols(void* h) {
#define RESOLVE(sym) do {                                                  \
        *(void**)(&R.sym) = dlsym(h, #sym);                                \
        if (!R.sym) {                                                      \
            fprintf(stderr, "aether host_racket: the library is missing "  \
                            "%s (is it Racket CS's embedding library?)\n", #sym); \
            return -1;                                                     \
        }                                                                  \
    } while (0)
    RESOLVE(racket_boot);
    RESOLVE(racket_eval);
    RESOLVE(racket_namespace_require);
    RESOLVE(Sstring_to_symbol);
    RESOLVE(Scons);
    RESOLVE(Sstring_utf8);
    RESOLVE(Sinteger);
    // Racket 8.14 added boot images given as bytes (and `bootN_data` to
    // racket_boot_arguments_t); Sregister_boot_file_bytes arrived with it.
    g_new_boot = dlsym(h, "Sregister_boot_file_bytes") != NULL;
    const char* force = getenv("AETHER_RACKET_BOOT_ABI");   // "old" or "new"
    if (force && !strcmp(force, "old")) g_new_boot = 0;
    if (force && !strcmp(force, "new")) g_new_boot = 1;
    return 0;
#undef RESOLVE
}

static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

static int exists(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

// The last (newest by name) match of a glob pattern, copied into out.
static int glob_last(const char* pattern, char* out, size_t n) {
    glob_t g;
    if (glob(pattern, 0, NULL, &g) != 0) return 0;
    int ok = 0;
    if (g.gl_pathc > 0) {
        snprintf(out, n, "%s", g.gl_pathv[g.gl_pathc - 1]);
        ok = 1;
    }
    globfree(&g);
    return ok;
}

// The path the library was loaded from (dladdr on racket_boot).
static void lib_dir(char* out, size_t n) {
    out[0] = '\0';
    Dl_info info;
    if (g_lib && dladdr((void*)R.racket_boot, &info) && info.dli_fname) {
        snprintf(out, n, "%s", info.dli_fname);
        char* slash = strrchr(out, '/');
        if (slash) *slash = '\0'; else out[0] = '\0';
    }
}

// Load the library on first use:
//   1. $AETHER_RACKET_SONAME, a name or path.
//   2. $AETHER_RACKET_HOME, the root of a Racket installation: its
//      lib/Racket.framework (macOS) or lib/libracketcs.so.
//   3. the usual places: the macOS distribution's framework (/Applications/
//      Racket v*), Homebrew, then libracketcs.so on the loader's path.
static int load_racket(void) {
    if (g_lib) return 0;

    const char* soname = getenv("AETHER_RACKET_SONAME");
    const char* home = getenv("AETHER_RACKET_HOME");
    char path[PATH_MAX];
    char first_err[512] = "";
    void* h = try_dlopen(soname);
    if (!h && soname && *soname) {
        // dlerror() clears the error it reports: keep the first, the one for
        // the name that was asked for.
        const char* e = dlerror();
        snprintf(first_err, sizeof first_err, "%s", e ? e : "");
    }
    if (!h && home && *home) {
        snprintf(path, sizeof path, "%s/lib/Racket.framework/Versions/*/Racket", home);
        if (glob_last(path, path, sizeof path)) h = try_dlopen(path);
        if (!h) { snprintf(path, sizeof path, "%s/lib/libracketcs.so", home); h = try_dlopen(path); }
        if (!h) { snprintf(path, sizeof path, "%s/lib/libracketcs.dylib", home); h = try_dlopen(path); }
        if (!h) { snprintf(path, sizeof path, "%s/libracketcs.so", home); h = try_dlopen(path); }
    }
    if (!h && glob_last("/Applications/Racket v*/lib/Racket.framework/Versions/*/Racket", path, sizeof path))
        h = try_dlopen(path);
    if (!h && glob_last("/opt/homebrew/lib/Racket.framework/Versions/*/Racket", path, sizeof path))
        h = try_dlopen(path);
    if (!h) h = try_dlopen("libracketcs.so");
    if (!h) h = try_dlopen("libracketcs.dylib");
    if (!h) {
        const char* last = dlerror();
        const char* why = first_err[0] ? first_err : last;
        fprintf(stderr,
            "aether host_racket: cannot dlopen Racket CS's library "
            "(tried $AETHER_RACKET_SONAME=%s, $AETHER_RACKET_HOME=%s, "
            "Racket.framework, libracketcs.so).\n"
            "  Install Racket CS and set AETHER_RACKET_SONAME to the library "
            "(Racket.framework/Versions/<v>/Racket on macOS; elsewhere "
            "libracketcs.so made from libracketcs.a, see racket/README.md).\n"
            "  dlerror: %s\n",
            soname ? soname : "(unset)", home ? home : "(unset)", why ? why : "(none)");
        return -1;
    }
    if (resolve_symbols(h) != 0) {
        dlclose(h);
        return -1;
    }
    g_lib = h;
    return 0;
}

// --- where the boot images, collects and config are ---------------------------

static char g_boot_dir[PATH_MAX], g_collects[PATH_MAX], g_config[PATH_MAX];
static char g_b1[PATH_MAX + 16], g_b2[PATH_MAX + 16], g_b3[PATH_MAX + 16];

static int has_boot(const char* dir) {
    char p[PATH_MAX + 16];
    snprintf(p, sizeof p, "%s/petite.boot", dir);
    if (!exists(p)) return 0;
    snprintf(p, sizeof p, "%s/scheme.boot", dir);
    if (!exists(p)) return 0;
    snprintf(p, sizeof p, "%s/racket.boot", dir);
    if (exists(p)) return 1;
    snprintf(p, sizeof p, "%s/racket.so", dir);
    return exists(p);
}

static int find_boot_dir(void) {
    const char* env = getenv("AETHER_RACKET_BOOT_DIR");
    if (env && *env) {
        snprintf(g_boot_dir, sizeof g_boot_dir, "%s", env);
        return has_boot(g_boot_dir) ? 0 : -1;
    }
    char ld[PATH_MAX], c[PATH_MAX + 64];
    lib_dir(ld, sizeof ld);
    const char* home = getenv("AETHER_RACKET_HOME");
    if (home && *home) {
        snprintf(c, sizeof c, "%s/lib/Racket.framework/Versions/*/boot", home);
        if (glob_last(c, g_boot_dir, sizeof g_boot_dir) && has_boot(g_boot_dir)) return 0;
        snprintf(c, sizeof c, "%s/lib/racket", home);
        if (has_boot(c)) { snprintf(g_boot_dir, sizeof g_boot_dir, "%s", c); return 0; }
        snprintf(c, sizeof c, "%s/lib", home);
        if (has_boot(c)) { snprintf(g_boot_dir, sizeof g_boot_dir, "%s", c); return 0; }
    }
    if (ld[0]) {
        const char* sub[] = {"/boot", "", "/racket"};
        for (int i = 0; i < 3; i++) {
            snprintf(c, sizeof c, "%s%s", ld, sub[i]);
            if (has_boot(c)) { snprintf(g_boot_dir, sizeof g_boot_dir, "%s", c); return 0; }
        }
    }
    const char* usual[] = {"/usr/lib/racket", "/usr/local/lib/racket", "/usr/lib64/racket"};
    for (int i = 0; i < 3; i++)
        if (has_boot(usual[i])) { snprintf(g_boot_dir, sizeof g_boot_dir, "%s", usual[i]); return 0; }
    return -1;
}

static void find_collects_and_config(void) {
    g_collects[0] = g_config[0] = '\0';
    const char* env = getenv("AETHER_RACKET_COLLECTS");
    char ld[PATH_MAX], c[PATH_MAX + 64];
    lib_dir(ld, sizeof ld);
    const char* home = getenv("AETHER_RACKET_HOME");
    if (env && *env) {
        snprintf(g_collects, sizeof g_collects, "%s", env);
    } else {
        if (home && *home) {
            snprintf(c, sizeof c, "%s/collects", home);
            if (exists(c)) snprintf(g_collects, sizeof g_collects, "%s", c);
            if (!g_collects[0]) {
                snprintf(c, sizeof c, "%s/share/racket/collects", home);
                if (exists(c)) snprintf(g_collects, sizeof g_collects, "%s", c);
            }
        }
        // Racket.framework/Versions/<v>/Racket: the installation root is four up.
        if (!g_collects[0] && ld[0]) {
            snprintf(c, sizeof c, "%s/../../../../collects", ld);
            if (exists(c)) snprintf(g_collects, sizeof g_collects, "%s", c);
        }
        const char* usual[] = {"/usr/share/racket/collects", "/usr/local/share/racket/collects",
                               "/opt/homebrew/share/racket/collects"};
        for (int i = 0; !g_collects[0] && i < 3; i++)
            if (exists(usual[i])) snprintf(g_collects, sizeof g_collects, "%s", usual[i]);
    }
    env = getenv("AETHER_RACKET_CONFIG");
    if (env && *env) {
        snprintf(g_config, sizeof g_config, "%s", env);
    } else {
        if (g_collects[0]) {
            snprintf(c, sizeof c, "%s/../etc", g_collects);
            if (exists(c)) snprintf(g_config, sizeof g_config, "%s", c);
        }
        const char* usual[] = {"/etc/racket", "/usr/local/etc/racket"};
        for (int i = 0; !g_config[0] && i < 2; i++)
            if (exists(usual[i])) snprintf(g_config, sizeof g_config, "%s", usual[i]);
    }
}

// --- a small s-expression reader, for the prelude and the few datums ----------
//
// Symbols, strings (with \\ \" \n), decimal integers, #t, #f, 'x, lists in
// () or []. Everything is built with Scons / Sstring_to_symbol / Sstring_utf8 /
// Sinteger, none of which runs Scheme.

static const char* rd_p;

static void rd_skip(void) {
    for (;;) {
        while (*rd_p == ' ' || *rd_p == '\t' || *rd_p == '\n' || *rd_p == '\r') rd_p++;
        if (*rd_p == ';') { while (*rd_p && *rd_p != '\n') rd_p++; continue; }
        return;
    }
}

static ptr rd_datum(void);

static ptr rd_list(char close) {
    ptr items[512];
    int n = 0;
    for (;;) {
        rd_skip();
        if (*rd_p == close) { rd_p++; break; }
        if (!*rd_p || n >= 512) { fprintf(stderr, "aether host_racket: bad prelude\n"); break; }
        items[n++] = rd_datum();
    }
    ptr out = Snil;
    while (n > 0) out = R.Scons(items[--n], out);
    return out;
}

static ptr rd_datum(void) {
    rd_skip();
    char c = *rd_p;
    if (c == '(') { rd_p++; return rd_list(')'); }
    if (c == '[') { rd_p++; return rd_list(']'); }
    if (c == '\'') {
        rd_p++;
        ptr d = rd_datum();
        return R.Scons(R.Sstring_to_symbol("quote"), R.Scons(d, Snil));
    }
    if (c == '"') {
        char buf[8192];
        size_t n = 0;
        rd_p++;
        while (*rd_p && *rd_p != '"' && n < sizeof buf - 1) {
            if (*rd_p == '\\') {
                rd_p++;
                buf[n++] = *rd_p == 'n' ? '\n' : *rd_p;
                if (*rd_p) rd_p++;
            } else {
                buf[n++] = *rd_p++;
            }
        }
        if (*rd_p == '"') rd_p++;
        buf[n] = '\0';
        return R.Sstring_utf8(buf, (iptr)n);
    }
    char tok[256];
    size_t n = 0;
    while (*rd_p && !strchr(" \t\r\n()[]\";", *rd_p) && n < sizeof tok - 1) tok[n++] = *rd_p++;
    tok[n] = '\0';
    if (!strcmp(tok, "#t")) return Strue;
    if (!strcmp(tok, "#f")) return Sfalse;
    char* end;
    long long v = strtoll(tok, &end, 10);
    if (n > 0 && *end == '\0') return R.Sinteger((iptr)v);
    return R.Sstring_to_symbol(tok);
}

static ptr datum_of(const char* text) {
    rd_p = text;
    return rd_datum();
}

// Evaluate every form of `text` in Racket's namespace.
static void eval_text(const char* text) {
    rd_p = text;
    for (;;) {
        rd_skip();
        if (!*rd_p) break;
        R.racket_eval(rd_datum());
    }
}

// --- the prelude: what the bridge defines in Racket's namespace ---------------
//
// aether-run runs code and returns a bytevector: "O" and what it printed, or
// "E", the message, a NUL and what it printed. There is no time limit: see
// racket/README.md ("Stopping a run"). Host functions (racket_register) are Racket procedures
// that go through a frame in C: the arguments are put there, the Aether
// function is called, and its results are read back.

static const char* PRELUDE =
"(define aether-host-kv (make-hash))\n"
"(define aether-rhombus-seq 0)\n"
"(define (aether-run-rhombus code)\n"
"  (dynamic-require 'rhombus #f)\n"
"  (set! aether-rhombus-seq (+ aether-rhombus-seq 1))\n"
"  (let ([name (make-resolved-module-path (string->symbol (format \"aether-rhombus-~a\" aether-rhombus-seq)))])\n"
"    (parameterize ([read-accept-reader #t] [read-accept-lang #t])\n"
"      (let ([stx (read-syntax 'aether (open-input-string (string-append \"#lang rhombus\\n\" code)))])\n"
"        (parameterize ([current-module-declare-name name]) (eval stx))\n"
"        (dynamic-require name #f)))))\n"
"(define (aether-run-body code lang)\n"
"  (if (eqv? lang 1)\n"
"      (aether-run-rhombus code)\n"
"      (eval (cons 'begin (port->list read (open-input-string code))))))\n"
"(define (aether-message e)\n"
"  (if (exn? e) (exn-message e) (format \"~a\" e)))\n"
"(define (aether-guard thunk)\n"
"  (with-handlers ([(lambda (e) #t) aether-message])\n"
"    (thunk)\n"
"    #f))\n"
"(define (aether-run code lang capture?)\n"
"  (let* ([err #f]\n"
"         [out (if capture?\n"
"                  (with-output-to-string\n"
"                    (lambda () (set! err (aether-guard (lambda () (aether-run-body code lang))))))\n"
"                  (begin\n"
"                    (set! err (aether-guard (lambda () (aether-run-body code lang))))\n"
"                    (flush-output (current-output-port))\n"
"                    \"\"))])\n"
"    (if err\n"
"        (bytes-append (bytes 69) (string->bytes/utf-8 err) (bytes 0) (string->bytes/utf-8 out))\n"
"        (bytes-append (bytes 79) (string->bytes/utf-8 out)))))\n"
"(define (aether-kv-set k v) (hash-set! aether-host-kv k v) (void))\n"
"(define (aether-kv-get k) (string->bytes/utf-8 (hash-ref aether-host-kv k \"\")))\n";

// --- the VM -----------------------------------------------------------------

static int g_booted = 0;
static int g_failed = 0;
static char g_last_error[1024];

static ptr SYM(const char* s) { return R.Sstring_to_symbol(s); }
static ptr LIST1(ptr a) { return R.Scons(a, Snil); }
static ptr LIST2(ptr a, ptr b) { return R.Scons(a, LIST1(b)); }
static ptr LIST3(ptr a, ptr b, ptr c) { return R.Scons(a, LIST2(b, c)); }

int racket_init(void) {
    if (g_booted) return 0;
    if (g_failed) return -1;
    if (load_racket() != 0) { g_failed = 1; return -1; }
    if (find_boot_dir() != 0) {
        fprintf(stderr,
            "aether host_racket: cannot find Racket CS's boot images "
            "(petite.boot, scheme.boot, racket.boot or racket.so).\n"
            "  Set AETHER_RACKET_BOOT_DIR to the directory that holds them "
            "(Racket.framework/Versions/<v>/boot on macOS, /usr/lib/racket "
            "on Debian and Ubuntu).\n");
        g_failed = 1;
        return -1;
    }
    find_collects_and_config();
    if (!g_collects[0]) {
        fprintf(stderr,
            "aether host_racket: cannot find Racket's collects directory; "
            "set AETHER_RACKET_COLLECTS.\n");
        g_failed = 1;
        return -1;
    }
    snprintf(g_b1, sizeof g_b1, "%s/petite.boot", g_boot_dir);
    snprintf(g_b2, sizeof g_b2, "%s/scheme.boot", g_boot_dir);
    snprintf(g_b3, sizeof g_b3, "%s/racket.boot", g_boot_dir);
    if (!exists(g_b3)) snprintf(g_b3, sizeof g_b3, "%s/racket.so", g_boot_dir);

    // embedded mode: boot, do not run a command line, do not exit.
    if (g_new_boot) {
        racket_boot_arguments_new_t a;
        memset(&a, 0, sizeof a);
        a.boot1_path = g_b1;
        a.boot2_path = g_b2;
        a.boot3_path = g_b3;
        a.exec_file = "aether";
        a.collects_dir = g_collects;
        a.config_dir = g_config[0] ? g_config : NULL;
        R.racket_boot(&a);
    } else {
        racket_boot_arguments_old_t a;
        memset(&a, 0, sizeof a);
        a.boot1_path = g_b1;
        a.boot2_path = g_b2;
        a.boot3_path = g_b3;
        a.exec_file = "aether";
        a.collects_dir = g_collects;
        a.config_dir = g_config[0] ? g_config : NULL;
        R.racket_boot(&a);
    }
    R.racket_namespace_require(SYM("racket"));
    R.racket_namespace_require(SYM("ffi/unsafe"));
    eval_text(PRELUDE);
    g_booted = 1;
    return 0;
}

void racket_finalize(void) {
    // Racket CS's embedding API has no teardown that allows a second boot:
    // the VM lives until the process exits.
}

// Copy a bytevector result out to a malloc'd, NUL-terminated string.
static char* bv_to_cstr(ptr bv, size_t* len) {
    if (!Sbytevectorp(bv)) {
        char* e = malloc(1);
        if (e) e[0] = '\0';
        if (len) *len = 0;
        return e;
    }
    iptr n = Sbytevector_length(bv);
    char* out = malloc((size_t)n + 1);
    if (!out) return NULL;
    if (n) memcpy(out, Sbytevector_data(bv), (size_t)n);
    out[n] = '\0';
    if (len) *len = (size_t)n;
    return out;
}

// racket_eval returns a list of the values: the first one.
static ptr first_value(ptr vlist) {
    return Spairp(vlist) ? Scar(vlist) : vlist;
}

static char* call_bytes(ptr datum) {
    return bv_to_cstr(first_value(R.racket_eval(datum)), NULL);
}

static void set_last_error(const char* msg) {
    snprintf(g_last_error, sizeof g_last_error, "%s", msg ? msg : "");
}

const char* racket_last_error(void) { return g_last_error; }

// lang 0 = Racket, 1 = Rhombus. Returns the raw result ("O..." or "E...") or
// NULL when the VM could not start.
static char* run_raw(int lang, const char* code, int capture) {
    if (!code) return NULL;
    if (racket_init() != 0) return NULL;
    // (aether-run "code" lang capture?)
    ptr datum = R.Scons(SYM("aether-run"),
                  LIST3(R.Sstring_utf8(code, -1), Sfixnum(lang), capture ? Strue : Sfalse));
    return call_bytes(datum);
}

static AetherString* evaluate_lang(int lang, const char* code) {
    char* r = run_raw(lang, code, 1);
    if (!r) return string_new("");
    AetherString* out;
    if (r[0] == 'E') {
        // "E" message NUL output: the output so far, then "error: message".
        const char* msg = r + 1;
        const char* printed = msg + strlen(msg) + 1;
        size_t cap = strlen(printed) + strlen(msg) + 16;
        char* both = malloc(cap);
        snprintf(both, cap, "%serror: %s", printed, msg);
        set_last_error(msg);
        out = string_new(both);
        free(both);
    } else {
        out = string_new(r + 1);
    }
    free(r);
    return out;
}

static int run_lang(int lang, const char* code) {
    char* r = run_raw(lang, code, 0);
    if (!r) return -1;
    int rc = 0;
    if (r[0] == 'E') {
        set_last_error(r + 1);
        fprintf(stderr, "[racket] %s\n", r + 1);
        rc = -1;
    }
    free(r);
    return rc;
}

AetherString* racket_evaluate(const char* code) { return evaluate_lang(0, code); }
int racket_run(const char* code) { return run_lang(0, code); }
AetherString* rhombus_evaluate(const char* code) { return evaluate_lang(1, code); }
int rhombus_run(const char* code) { return run_lang(1, code); }

// --- sandboxed runs -----------------------------------------------------------

static int run_sandboxed_lang(int lang, void* perms, const char* code) {
    if (!code) return -1;
    if (racket_init() != 0) return -1;
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;
    aether_sandbox_push(perms);
    int rc = run_lang(lang, code);
    aether_sandbox_pop();
    return rc;
}

int racket_run_sandboxed(void* perms, const char* code) { return run_sandboxed_lang(0, perms, code); }
int rhombus_run_sandboxed(void* perms, const char* code) { return run_sandboxed_lang(1, perms, code); }

// --- the shared map -----------------------------------------------------------
//
// aether-map-get and aether-map-put are Racket procedures wrapping the two C
// functions below (ffi/unsafe, the addresses as integers).

static uint64_t g_map_token = 0;
static int g_map_procs = 0;

static const char* host_map_get(const char* key) {
    if (!g_map_token || !key) return NULL;
    return aether_shared_map_get_by_token(g_map_token, key);
}
static void host_map_put(const char* key, const char* value) {
    if (!g_map_token || !key || !value) return;
    aether_shared_map_put_by_token(g_map_token, key, value);
}

// (define NAME (cast (cast ADDR _intptr _pointer) _fpointer (_fun ... )))
static void define_c_function(const char* name, void* addr, ptr fun_type) {
    ptr inner = R.Scons(SYM("cast"), LIST3(R.Sinteger((iptr)addr), SYM("_intptr"), SYM("_pointer")));
    ptr outer = R.Scons(SYM("cast"), LIST3(inner, SYM("_fpointer"), fun_type));
    R.racket_eval(LIST3(SYM("define"), SYM(name), outer));
}

static int install_map_procs(void) {
    if (g_map_procs) return 0;
    if (racket_init() != 0) return -1;
    eval_text(
        "(define aether-map-get-raw #f)\n"
        "(define aether-map-put #f)\n"
        "(define (aether-map-get k) (or (aether-map-get-raw k) \"\"))\n");
    define_c_function("aether-map-get-raw", (void*)&host_map_get,
        datum_of("(_fun _string/utf-8 -> _string/utf-8)"));
    define_c_function("aether-map-put", (void*)&host_map_put,
        datum_of("(_fun _string/utf-8 _string/utf-8 -> _void)"));
    g_map_procs = 1;
    return 0;
}

static int run_with_map_lang(int lang, void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    if (racket_init() != 0) return -1;
    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);
    if (install_map_procs() != 0) return -1;
    g_map_token = map_token;
    int rc = run_sandboxed_lang(lang, perms, code);
    g_map_token = 0;
    return rc;
}

int racket_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    return run_with_map_lang(0, perms, code, map_token);
}
int rhombus_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    return run_with_map_lang(1, perms, code, map_token);
}

// --- the string-only k-v map over a hash in the VM ------------------------------

int racket_set(const char* key, const char* value) {
    if (!key || !value) return -1;
    if (racket_init() != 0) return -1;
    R.racket_eval(R.Scons(SYM("aether-kv-set"),
                    LIST2(R.Sstring_utf8(key, -1), R.Sstring_utf8(value, -1))));
    return 0;
}

AetherString* racket_get(const char* key) {
    if (!key) return string_new("");
    if (racket_init() != 0) return string_new("");
    char* c = call_bytes(R.Scons(SYM("aether-kv-get"), LIST1(R.Sstring_utf8(key, -1))));
    if (!c) return string_new("");
    AetherString* out = string_new(c);
    free(c);
    return out;
}

int rhombus_set(const char* k, const char* v) { return racket_set(k, v); }
AetherString* rhombus_get(const char* k) { return racket_get(k); }
int rhombus_init(void) { return racket_init(); }
void rhombus_finalize(void) { racket_finalize(); }

// --- host functions: Racket calling Aether ---------------------------------------
//
// racket_register(name, fn) defines `name` as a Racket procedure. A call puts
// its arguments (strings, integers, booleans) in the frame below, runs the
// Aether function (`int fn(void* vm)`, which reads them with racket_arg_* and
// pushes results with racket_push_*, returning how many), and gives the
// results back as the procedure's values. racket_raise_error makes the
// procedure raise an exception with the message. One frame, so a host
// function must not itself call back into the VM.

#define MAX_VALS 16
#define MAX_HOST_FNS 64

typedef struct { int type; long long i; char* s; } val_t;     // 0 nil, 1 bool, 2 int, 3 str
static val_t g_args[MAX_VALS];
static int g_nargs = 0;
static val_t g_res[MAX_VALS];
static int g_nres = 0;
static char g_raised[1024];
static int g_is_raised = 0;

typedef int (*host_fn_t)(void*);
static host_fn_t g_host_fns[MAX_HOST_FNS];
static int g_n_host_fns = 0;

static void free_vals(val_t* v, int n) {
    for (int i = 0; i < n; i++) { free(v[i].s); v[i].s = NULL; v[i].type = 0; }
}

static void fr_reset(void) {
    free_vals(g_args, g_nargs);
    free_vals(g_res, g_nres);
    g_nargs = g_nres = 0;
    g_is_raised = 0;
}
static void fr_arg_str(const char* s) {
    if (g_nargs >= MAX_VALS) return;
    g_args[g_nargs].type = 3;
    g_args[g_nargs].s = strdup(s ? s : "");
    g_nargs++;
}
static void fr_arg_int(int64_t v) {
    if (g_nargs >= MAX_VALS) return;
    g_args[g_nargs].type = 2;
    g_args[g_nargs].i = v;
    g_nargs++;
}
static void fr_arg_bool(int v) {
    if (g_nargs >= MAX_VALS) return;
    g_args[g_nargs].type = 1;
    g_args[g_nargs].i = v != 0;
    g_nargs++;
}
// Runs host function idx: how many values it returned, or -1 if it raised.
static int fr_call(int idx) {
    if (idx < 0 || idx >= g_n_host_fns) return -1;
    g_nres = 0;
    int n = g_host_fns[idx](&g_nargs);
    if (g_is_raised) return -1;
    if (n < 0) n = 0;
    if (n > g_nres) n = g_nres;
    return n;
}
static int fr_rtype(int i) { return i >= 0 && i < g_nres ? g_res[i].type : 0; }
static const char* fr_rstr(int i) { return i >= 0 && i < g_nres && g_res[i].s ? g_res[i].s : ""; }
static int64_t fr_rint(int i) { return i >= 0 && i < g_nres ? g_res[i].i : 0; }
static const char* fr_err(void) { return g_raised; }

int racket_register(const char* name, void* host_fn) {
    if (!name || !host_fn) return -1;
    if (racket_init() != 0) return -1;
    static int frame_installed = 0;
    if (!frame_installed) {
        define_c_function("aether-fr-reset", (void*)&fr_reset, datum_of("(_fun -> _void)"));
        define_c_function("aether-fr-arg-str", (void*)&fr_arg_str, datum_of("(_fun _string/utf-8 -> _void)"));
        define_c_function("aether-fr-arg-int", (void*)&fr_arg_int, datum_of("(_fun _int64 -> _void)"));
        define_c_function("aether-fr-arg-bool", (void*)&fr_arg_bool, datum_of("(_fun _int -> _void)"));
        define_c_function("aether-fr-call", (void*)&fr_call, datum_of("(_fun _int -> _int)"));
        define_c_function("aether-fr-rtype", (void*)&fr_rtype, datum_of("(_fun _int -> _int)"));
        define_c_function("aether-fr-rstr", (void*)&fr_rstr, datum_of("(_fun _int -> _string/utf-8)"));
        define_c_function("aether-fr-rint", (void*)&fr_rint, datum_of("(_fun _int -> _int64)"));
        define_c_function("aether-fr-err", (void*)&fr_err, datum_of("(_fun -> _string/utf-8)"));
        eval_text(
            "(define (aether-host-fn idx)\n"
            "  (lambda args\n"
            "    (aether-fr-reset)\n"
            "    (for-each (lambda (a)\n"
            "                (cond [(string? a) (aether-fr-arg-str a)]\n"
            "                      [(boolean? a) (aether-fr-arg-bool (if a 1 0))]\n"
            "                      [(exact-integer? a) (aether-fr-arg-int a)]\n"
            "                      [else (raise (exn:fail \"a host function takes strings, integers and booleans\" (current-continuation-marks)))]))\n"
            "              args)\n"
            "    (let ([n (aether-fr-call idx)])\n"
            "      (if (< n 0)\n"
            "          (raise (exn:fail (aether-fr-err) (current-continuation-marks)))\n"
            "          (let ([rs (for/list ([i (in-range n)])\n"
            "                      (case (aether-fr-rtype i)\n"
            "                        [(0) #f]\n"
            "                        [(1) (not (zero? (aether-fr-rint i)))]\n"
            "                        [(2) (aether-fr-rint i)]\n"
            "                        [else (aether-fr-rstr i)]))])\n"
            "            (if (null? rs) (void) (apply values rs)))))))\n");
        frame_installed = 1;
    }
    for (int i = 0; i < g_n_host_fns; i++) {
        if (g_host_fns[i] == (host_fn_t)host_fn) {
            // already registered under another name: reuse its slot
            R.racket_eval(LIST3(SYM("define"), SYM(name), LIST2(SYM("aether-host-fn"), Sfixnum(i))));
            return 0;
        }
    }
    if (g_n_host_fns >= MAX_HOST_FNS) return -1;
    g_host_fns[g_n_host_fns] = (host_fn_t)host_fn;
    R.racket_eval(LIST3(SYM("define"), SYM(name), LIST2(SYM("aether-host-fn"), Sfixnum(g_n_host_fns))));
    g_n_host_fns++;
    return 0;
}

int racket_arg_count(void* vm) { (void)vm; return g_nargs; }
int racket_arg_type(void* vm, int i) { (void)vm; return i >= 1 && i <= g_nargs ? g_args[i - 1].type : 0; }
const char* racket_arg_str(void* vm, int i) {
    (void)vm;
    static char numbuf[32];
    if (i < 1 || i > g_nargs) return "";
    val_t* a = &g_args[i - 1];
    if (a->type == 3) return a->s ? a->s : "";
    snprintf(numbuf, sizeof numbuf, "%lld", a->i);
    return numbuf;
}
int64_t racket_arg_int(void* vm, int i) {
    (void)vm;
    if (i < 1 || i > g_nargs) return 0;
    val_t* a = &g_args[i - 1];
    return a->type == 3 ? strtoll(a->s ? a->s : "0", NULL, 10) : a->i;
}
int racket_arg_bool(void* vm, int i) { return racket_arg_int(vm, i) != 0; }

static val_t* push_slot(void) {
    if (g_nres >= MAX_VALS) return NULL;
    val_t* v = &g_res[g_nres++];
    v->s = NULL;
    v->i = 0;
    return v;
}
void racket_push_int(void* vm, int64_t x) { (void)vm; val_t* v = push_slot(); if (v) { v->type = 2; v->i = x; } }
void racket_push_str(void* vm, const char* s) { (void)vm; val_t* v = push_slot(); if (v) { v->type = 3; v->s = strdup(s ? s : ""); } }
void racket_push_bool(void* vm, int b) { (void)vm; val_t* v = push_slot(); if (v) { v->type = 1; v->i = b != 0; } }
void racket_push_nil(void* vm) { (void)vm; val_t* v = push_slot(); if (v) v->type = 0; }
int racket_raise_error(void* vm, const char* msg) {
    (void)vm;
    snprintf(g_raised, sizeof g_raised, "%s", msg ? msg : "error");
    g_is_raised = 1;
    return 0;
}
