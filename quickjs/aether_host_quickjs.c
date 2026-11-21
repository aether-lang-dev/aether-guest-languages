// aether_host_quickjs.c: the QuickJS bridge's C, compiled into the program
// that imports `quickjs` (module.ae's @source).
//
// The engine is aether's contrib.quickjs (module.ae imports it, which
// compiles its C and the QuickJS amalgamation into the same program). This
// file adds the two-way VM the Lua, Python, Duktape and Tcl bridges have (a
// VM per handle, host functions by name, globals, an eval with a typed
// result, typed argument and result readers), on the engine's handle API.
// No engine code is copied: every value lives in the engine's handle table.
//
// Ownership. The caller owns one thing: the VM, from vm_new until vm_free
// (module.ae declares vm_free's parameter @frees, so the compiler refuses a
// use of the local after it). Every engine handle the VM takes is the VM's
// and is released by it: the held result (until the next eval or
// pop_result), a callback's arguments (the engine's, borrowed for the call),
// a callback's pushed value (handed to the engine when the callback returns
// it, released when it does not), and the host functions' closures (the
// engine's, freed with the runtime). Strings the readers return are Aether
// strings made from the engine's bytes and their length (a NUL in them
// crosses), each the caller's own; the VM keeps nothing of them. A std.bytes
// handle a reader returns is the caller's.
//
// Strings and NUL. JavaScript strings hold a NUL like any other character.
// contrib.quickjs reads them out with their length (qjs_to_string), but takes
// them in as C strings (qjs_string, qjs_eval, qjs_throw, qjs_parse_json: a NUL
// ends them), and keeps an error as C text (fail_). So: what the engine hands
// out is returned with its length; a host string given as an Aether string is
// carried in by its length (a string with a NUL is made through JSON.parse,
// which writes it as \u0000, ASCII); an error is read with its length by
// catching it again in JavaScript (module.ae's helpers); a raise_error message
// with a NUL is thrown by a JavaScript helper. What the engine cannot be given
// at all is refused, loudly, naming the reason: script text (qjs_eval), JSON
// text (invalid with a raw NUL anyway), the names of globals and host functions
// (a different name is worse than none) and the shared map's keys and values
// (aether_shared_map_* store C strings). asks/contrib-quickjs-length-aware-strings.md
// and asks/shared-map-nul.md propose the length-aware engine and runtime calls.
#include "aether_host_quickjs.h"
#ifndef AETHER_HAS_SANDBOX
#define AETHER_HAS_SANDBOX 1
#endif
// The installed runtime's headers (include/aether/runtime, include/aether/std),
// which `ae build` puts on the include path of every C file it compiles into
// a program.
#include "aether_sandbox.h"
#include "aether_shared_map.h"
#include "aether_bytes.h"
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the engine: contrib/quickjs/aether_quickjs.c ---------------------------
// Declared by hand (there is no shared header); these must match aether's
// definitions exactly, and a release that changes them fails the specs.
extern void* qjs_new(int memory_kb, int stack_kb);
extern void qjs_free(void* q);
extern int64_t qjs_now_ns(void);
extern void qjs_set_deadline_ns(void* q, int64_t ns);
extern void qjs_set_cancel_cell(void* q, void* cell);
extern int qjs_interrupted(void* q);
extern void qjs_clear_interrupted(void* q);
extern int qjs_eval(void* q, const char* code, const char* filename, int flags);
extern int qjs_run_jobs(void* q);
extern const char* qjs_error(void* q);
extern int qjs_throw(void* q, const char* kind, const char* msg);
extern void qjs_release(void* q, int h);
extern int qjs_global(void* q);
extern int qjs_null(void* q);
extern int qjs_bool(void* q, int b);
extern int qjs_int(void* q, int n);
extern int qjs_float(void* q, double d);
extern int qjs_string(void* q, const char* s);
extern int qjs_object(void* q);
extern int qjs_array(void* q);
extern int qjs_parse_json(void* q, const char* text);
extern const char* qjs_to_json(void* q, int h);
extern int qjs_to_string(void* q, int h);
extern const void* qjs_scratch(void* q);
extern double qjs_to_float(void* q, int h);
extern int qjs_to_bool(void* q, int h);
extern const char* qjs_type(void* q, int h);
extern int qjs_get(void* q, int obj, const char* name);
extern int qjs_set(void* q, int obj, const char* name, int v);
extern int qjs_get_index(void* q, int obj, int i);
extern int qjs_set_index(void* q, int obj, int i, int v);
extern int qjs_length(void* q, int obj);
extern int qjs_u8_length(void* q, int h);
extern const void* qjs_u8_data(void* q, int h);
extern int qjs_new_uint8array(void* q, const void* data, int n);
extern int qjs_call(void* q, int f, int this_h, int args);
extern int qjs_rethrow(void* q);
extern int qjs_dup(void* q, int h);
// The engine's interrupt reasons (contrib.quickjs INTERRUPTED_*).
#define QJS_INTERRUPT_DEADLINE 1
#define QJS_INTERRUPT_CANCEL 2
#define QJS_INTERRUPT_STOP 3

// std.list and std.string, for vm_set_global_strlist and the strings that
// cross (an Aether string is its bytes and its length; a plain char* reads the
// same through these).
extern int list_size(void*);
extern void* list_get_raw(void*, int);
extern const char* aether_string_data(const void*);
extern size_t aether_string_length(const void*);
extern void* string_new_with_length(const char* data, size_t length);

// ---- typed-value tags (module.ae's QJS_T*: Python's numbering) -------------
#define AE_QJS_TNIL   0
#define AE_QJS_TBOOL  1
#define AE_QJS_TINT   2
#define AE_QJS_TSTR   3
#define AE_QJS_TTABLE 4
#define AE_QJS_TERR   5
#define AE_QJS_TOTHER 6
#define AE_QJS_TBYTES 7
#define AE_QJS_TFLOAT 8

// What quickjs_raise_error returns, so a host function can `return
// quickjs.raise_error(vm, msg)`. The error is thrown by the dispatch after the
// host function has returned, so its defers run.
#define AE_QJS_RAISE (-1)

// The state of the callback now running. Saved and restored around each
// callback, so a nested eval's callbacks do not clobber it.
typedef struct {
    int args;          // the engine's arguments array (borrowed for the call)
    int pushed;        // the last push (the VM's until handed to the engine)
    char* raise;       // set by raise_error (and by a refusal in a callback)
    size_t raise_len;
} AeQjsFrame;

typedef struct AeQjsVm {
    void* q;               // the engine runtime (contrib.quickjs), the VM's
    // The last eval's result: a handle the VM holds until the next eval or
    // pop_result, its tag, and a copy of its text (a string result, or the
    // error's message). result_full is the error with its stack, or the
    // result's JSON once asked for.
    int result;
    int result_type;
    char* result_str;
    size_t result_str_len;
    char* result_full;     // an error with its stack, for run's report
    size_t result_full_len;
    // JavaScript the VM keeps handles to, none visible to a script: the
    // function that throws the error the engine's rethrow brings back
    // (rethrow_fn, made by module.ae), the one that catches it as text
    // (catcher) and the one that throws a message with a NUL (thrower).
    int rethrow_fn;
    int catcher;
    int thrower;
    int joiner;            // joins an array of strings with a NUL: new_string's
    AeQjsFrame frame;
    int64_t time_limit_ns; // per outermost eval; 0 for none
    int depth;             // evals running on this VM (nested ones count)
    void* user;            // the host's, for its callbacks (vm_user)
    uint64_t map_token;    // run_sandboxed_with_map's map, for its callbacks
} AeQjsVm;

static char* dup_str(const char* s) {
    size_t n = strlen(s ? s : "");
    char* d = (char*)malloc(n + 1);
    if (d) memcpy(d, s ? s : "", n + 1);
    return d;
}

static const char* nz(const char* s) { return s ? s : ""; }

// A string the host gave (an Aether string, or a plain char*): its bytes and
// its length, NULL read as "".
static const char* host_bytes(const void* v, size_t* n) {
    const char* d = aether_string_data(v);
    if (!d) { *n = 0; return ""; }
    *n = aether_string_length(v);
    return d;
}

static int has_nul(const char* d, size_t n) { return n > 0 && memchr(d, 0, n) != NULL; }

// An Aether string made from bytes and a length, for the host (NUL and all).
static void* out_str(const char* d, size_t n) {
    return string_new_with_length(d ? d : "", d ? n : 0);
}

// What a string the bridge cannot pass on says, "<what>: refused: it holds an
// embedded NUL; <why>" (the words tcl/ uses, which tests/boundary.py reads as a
// documented refusal).
#define REFUSE(what, why) what ": refused: it holds an embedded NUL; " why
#define WHY_CODE "contrib.quickjs reads script text as a C string, which a NUL would end early; write \\0 inside a string literal instead"
#define WHY_NAME "a name is a C string, and a NUL would cut it into a different name"
#define WHY_JSON "JSON text cannot hold a raw NUL; write \\u0000 inside a string instead"
#define WHY_MAP "the shared map stores C strings (aether_shared_map_put), which a NUL would cut"

// ---- the VM -----------------------------------------------------------------

// A VM on a runtime of its own; memory_kb caps its heap (0: no cap). NULL
// when the runtime cannot be made.
void* qjsb_new(int memory_kb) {
    AeQjsVm* vm = (AeQjsVm*)calloc(1, sizeof(AeQjsVm));
    if (!vm) return NULL;
    vm->q = qjs_new(memory_kb > 0 ? memory_kb : 0, 0);
    if (!vm->q) { free(vm); return NULL; }
    vm->result_type = AE_QJS_TNIL;
    return vm;
}

// The JavaScript the VM keeps for the strings the engine hands over as C text
// (set_error, throw_with_nul, new_string). rethrow_fn is a host function module.ae made,
// whose body is the engine's rethrow; the others are small functions no
// script can see. Whatever fails to be made leaves the VM on the engine's own
// text (an error cut at a NUL), which only happens on a heap too small for
// four functions.
void qjsb_set_helpers(void* v, int rethrow_fn) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return;
    vm->rethrow_fn = rethrow_fn > 0 ? rethrow_fn : 0;
    vm->catcher = qjs_eval(vm->q, "(function (f) { try { f() } catch (e) { try { return String(e) } catch (_) { return undefined } } })", "<bridge>", 0);
    if (vm->catcher < 0) vm->catcher = 0;
    vm->thrower = qjs_eval(vm->q, "(function (m) { throw new Error(m) })", "<bridge>", 0);
    if (vm->thrower < 0) vm->thrower = 0;
    vm->joiner = qjs_eval(vm->q, "(function (a) { return a.join('\\0') })", "<bridge>", 0);
    if (vm->joiner < 0) vm->joiner = 0;
}

// The engine runtime, for module.ae's registration; NULL for no VM.
void* qjsb_engine(void* v) { return v ? ((AeQjsVm*)v)->q : NULL; }

static void clear_result(AeQjsVm* vm) {
    if (vm->result > 0) qjs_release(vm->q, vm->result);
    vm->result = 0;
    vm->result_type = AE_QJS_TNIL;
    free(vm->result_str);
    vm->result_str = NULL;
    vm->result_str_len = 0;
    free(vm->result_full);
    vm->result_full = NULL;
    vm->result_full_len = 0;
}

// Refused while one of the VM's own evals is running (a callback freeing the
// VM it was called from): the eval's caller still holds the VM and frees it.
void quickjs_vm_free(void* v) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm || vm->depth > 0) return;
    clear_result(vm);
    // Every handle the engine still holds, the context, the runtime and the
    // host functions' closures (their captured environments too).
    qjs_free(vm->q);
    free(vm);
}

// 1 while an eval of this VM is running (so vm_free would be refused).
int quickjs_vm_busy(void* v) { return v && ((AeQjsVm*)v)->depth > 0 ? 1 : 0; }

// ---- reading a value --------------------------------------------------------

// A JavaScript number as int64 without undefined behaviour: NaN reads 0, a
// value outside int64's range saturates, a fraction is truncated.
static int64_t num_to_i64(double d) {
    if (d != d) return 0;
    if (d >= 9223372036854775807.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)d;
}

// A BigInt's decimal text (no JavaScript runs for a primitive), or NULL.
static const char* bigint_text(void* q, int h) {
    int n = qjs_to_string(q, h);
    return n >= 0 ? (const char*)qjs_scratch(q) : NULL;
}

// JavaScript has one number type: a number with no fraction that fits in
// 64 bits is an int, anything else (a fraction, NaN, an infinity, 2**63 and
// beyond) a float.
static int classify(void* q, int h) {
    const char* t = qjs_type(q, h);
    if (!strcmp(t, "undefined") || !strcmp(t, "null")) return AE_QJS_TNIL;
    if (!strcmp(t, "boolean")) return AE_QJS_TBOOL;
    if (!strcmp(t, "number")) {
        double d = qjs_to_float(q, h);
        if (isfinite(d) && d == trunc(d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0)
            return AE_QJS_TINT;
        return AE_QJS_TFLOAT;
    }
    if (!strcmp(t, "string")) return AE_QJS_TSTR;
    if (!strcmp(t, "array")) return AE_QJS_TTABLE;
    if (!strcmp(t, "object")) return qjs_u8_length(q, h) >= 0 ? AE_QJS_TBYTES : AE_QJS_TTABLE;
    // bigint, symbol, function, error (as a value), promise
    return AE_QJS_TOTHER;
}

static int64_t value_int(void* q, int h) {
    const char* t = qjs_type(q, h);
    if (!strcmp(t, "number")) return num_to_i64(qjs_to_float(q, h));
    if (!strcmp(t, "boolean")) return qjs_to_bool(q, h);
    if (!strcmp(t, "bigint")) {
        // One that fits in 64 bits; 0 for one that does not (as Python's
        // past-64-bit int), its nearest double through value_double.
        const char* s = bigint_text(q, h);
        if (!s) return 0;
        errno = 0;
        char* end = NULL;
        long long v = strtoll(s, &end, 10);
        if (errno != 0 || !end || *end) return 0;
        return (int64_t)v;
    }
    return 0;
}

static double value_double(void* q, int h) {
    const char* t = qjs_type(q, h);
    if (!strcmp(t, "number")) return qjs_to_float(q, h);
    if (!strcmp(t, "boolean")) return qjs_to_bool(q, h);
    if (!strcmp(t, "bigint")) {
        const char* s = bigint_text(q, h);
        return s ? strtod(s, NULL) : 0.0;
    }
    return 0.0;
}

static int value_bool(void* q, int h) {
    return !strcmp(qjs_type(q, h), "boolean") && qjs_to_bool(q, h) ? 1 : 0;
}

// A string's UTF-8 text and length (a NUL kept: the engine counts bytes), as
// a new malloc'd copy; NULL when h is no string. A lone surrogate, which
// UTF-8 cannot encode, is U+FFFD (the engine's own conversion), and a
// character beyond the BMP is its four-byte UTF-8 form, not CESU-8.
static char* value_str(void* q, int h, size_t* len) {
    if (strcmp(qjs_type(q, h), "string") != 0) return NULL;
    int n = qjs_to_string(q, h);
    if (n < 0) return NULL;
    char* s = (char*)malloc((size_t)n + 1);
    if (!s) return NULL;
    memcpy(s, qjs_scratch(q), (size_t)n);
    s[n] = '\0';
    // The engine writes a lone surrogate as the three bytes ED A0..BF xx (a
    // surrogate code point, which is not UTF-8: WTF-8). The host gets U+FFFD
    // (EF BF BD, the same length) instead, so every string handed out is valid
    // UTF-8, as the other bridges' are; JSON (vm_result_json) keeps it exactly.
    for (int i = 0; i + 2 < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xED && ((unsigned char)s[i + 1] & 0xE0) == 0xA0) { s[i] = (char)0xEF; s[i + 1] = (char)0xBF; s[i + 2] = (char)0xBD; i += 2; }
        else if (c >= 0xC0) i += c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    }
    if (len) *len = (size_t)n;
    return s;
}

// A JavaScript string from bytes and a length. qjs_string takes a C string,
// which a NUL ends, so a string with a NUL is made in pieces: each piece
// between NULs by qjs_string (so the engine decodes it as it does any string:
// a bad byte is U+FFFD, a surrogate pair written as CESU-8 is joined, a lone
// one is kept), and the pieces joined with a NUL by a JavaScript function the
// VM keeps (module.ae's note on helpers). -1 when the engine cannot make it.
static int new_string(AeQjsVm* vm, const char* d, size_t n) {
    void* q = vm->q;
    if (!has_nul(d, n)) return qjs_string(q, d);
    if (vm->joiner <= 0) return -1;
    int arr = qjs_array(q);
    if (arr <= 0) return -1;
    int ok = 1;
    int idx = 0;
    size_t start = 0;
    for (size_t i = 0; i <= n && ok; i++) {
        if (i < n && d[i] != '\0') continue;
        char* piece = (char*)malloc(i - start + 1);
        if (!piece) { ok = 0; break; }
        memcpy(piece, d + start, i - start);
        piece[i - start] = '\0';
        int s = qjs_string(q, piece);
        free(piece);
        if (s <= 0) { ok = 0; break; }
        if (qjs_set_index(q, arr, idx++, s) < 0) ok = 0;
        qjs_release(q, s);
        start = i + 1;
    }
    int out = -1;
    if (ok) {
        int args = qjs_array(q);
        if (args > 0) {
            qjs_set_index(q, args, 0, arr);
            out = qjs_call(q, vm->joiner, 0, args);
            qjs_release(q, args);
        }
    }
    qjs_release(q, arr);
    return out;
}

// The bytes of a Uint8Array as a new std.bytes handle (the caller's); NULL
// for anything else. A string is never bytes.
static void* value_bytes(void* q, int h) {
    int n = qjs_u8_length(q, h);
    if (n < 0) return NULL;
    const void* d = qjs_u8_data(q, h);
    return aether_bytes_from_ptr((void*)d, n);
}

// ---- running code -----------------------------------------------------------

// The engine's error text without its stack: "TypeError: x" from
// "TypeError: x\n    at f (<eval>:1:3)". Only the fallback now (see
// caught_text): contrib.quickjs gives the text with the stack appended, not
// the exception value, so a message that itself holds "\n    at " is cut there
// (asks/contrib-quickjs-op-budget-and-exception-handle.md).
static char* message_of(const char* full, size_t* len) {
    const char* cut = strstr(full, "\n    at ");
    size_t n = cut ? (size_t)(cut - full) : strlen(full);
    char* m = (char*)malloc(n + 1);
    if (!m) return NULL;
    memcpy(m, full, n);
    m[n] = '\0';
    *len = n;
    return m;
}

// String(e) of the exception the last failed eval left, with its length. The
// engine records an error as C text (a NUL ends it), but keeps the value, and
// qjs_rethrow throws that value again: the catcher calls a function that does,
// in a try, and gives back String(e), whose bytes and length the engine reads
// out whole. NULL when it cannot (a heap that is full, an exception the script
// cannot catch, a String(e) that throws): the engine's text is used instead.
// A user toString on a thrown object runs a second time here.
static char* caught_text(AeQjsVm* vm, size_t* len) {
    void* q = vm->q;
    if (vm->rethrow_fn <= 0 || vm->catcher <= 0) return NULL;
    int arr = qjs_array(q);
    if (arr <= 0) return NULL;
    int f = qjs_dup(q, vm->rethrow_fn);
    if (f > 0) { qjs_set_index(q, arr, 0, f); qjs_release(q, f); }
    int r = qjs_call(q, vm->catcher, 0, arr);
    qjs_release(q, arr);
    if (r < 0) return NULL;
    char* s = value_str(q, r, len);
    qjs_release(q, r);
    return s;
}

static void set_error_text(AeQjsVm* vm, const char* text, size_t n) {
    vm->result_type = AE_QJS_TERR;
    vm->result_str = (char*)malloc(n + 1);
    if (vm->result_str) { memcpy(vm->result_str, text, n); vm->result_str[n] = '\0'; vm->result_str_len = n; }
    vm->result_full = (char*)malloc(n + 1);
    if (vm->result_full) { memcpy(vm->result_full, text, n); vm->result_full[n] = '\0'; vm->result_full_len = n; }
}

static void set_error(AeQjsVm* vm, int interrupted) {
    char* full = dup_str(nz(qjs_error(vm->q)));
    size_t flen = full ? strlen(full) : 0;
    const char* why = NULL;
    if (interrupted == QJS_INTERRUPT_DEADLINE) why = "InternalError: interrupted: time limit exceeded";
    else if (interrupted == QJS_INTERRUPT_CANCEL) why = "InternalError: interrupted: cancelled";
    else if (interrupted == QJS_INTERRUPT_STOP) why = "InternalError: interrupted: stopped";
    vm->result_type = AE_QJS_TERR;
    if (why) {
        vm->result_full = full;
        vm->result_full_len = flen;
        vm->result_str = dup_str(why);
        vm->result_str_len = strlen(why);
        return;
    }
    size_t n = 0;
    char* msg = caught_text(vm, &n);
    if (!msg) msg = message_of(full ? full : "", &n);
    vm->result_str = msg;
    vm->result_str_len = msg ? n : 0;
    if (msg && has_nul(msg, n) && full) {
        // The engine's text is cut at the NUL; the message with its length,
        // then the stack lines the engine wrote after the cut.
        const char* skip = full + strnlen(msg, n);
        const char* tail = strstr(skip, "\n    at ");
        size_t tl = tail ? strlen(tail) : 0;
        vm->result_full = (char*)malloc(n + tl + 1);
        if (vm->result_full) {
            memcpy(vm->result_full, msg, n);
            if (tl) memcpy(vm->result_full + n, tail, tl);
            vm->result_full[n + tl] = '\0';
            vm->result_full_len = n + tl;
        }
        free(full);
    } else {
        vm->result_full = full;
        vm->result_full_len = flen;
    }
}

// Runs code at global level, then the promise jobs it queued. 0 with the
// script's completion value as the result (statements give undefined), or
// -1 with the error as the result (QJS_TERR, its message as the string). A
// time limit applies to the outermost eval and everything it runs, a nested
// eval (from a callback) included, which neither renews nor removes it. Script
// text with a NUL in it is refused (-1, and says why): the engine takes it as
// a C string.
int quickjs_vm_eval(void* v, const void* code) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm || !code) return -1;
    void* q = vm->q;
    size_t clen;
    const char* cd = host_bytes(code, &clen);
    clear_result(vm);
    if (has_nul(cd, clen)) {
        set_error_text(vm, REFUSE("the script", WHY_CODE), strlen(REFUSE("the script", WHY_CODE)));
        return -1;
    }
    int outer = vm->depth == 0;
    if (outer) {
        qjs_clear_interrupted(q);
        qjs_set_deadline_ns(q, vm->time_limit_ns > 0 ? qjs_now_ns() + vm->time_limit_ns : 0);
    }
    vm->depth++;
    int h = qjs_eval(q, cd, "<eval>", 0);
    if (h >= 0 && outer && qjs_run_jobs(q) < 0) {
        qjs_release(q, h);
        h = -1;
    }
    vm->depth--;
    // A nested eval may have left a result of its own behind (a callback
    // that did not pop it): this eval's result replaces it.
    clear_result(vm);
    int rc = 0;
    if (h < 0) {
        set_error(vm, qjs_interrupted(q));
        rc = -1;
    } else {
        vm->result = h;
        vm->result_type = classify(q, h);
        if (vm->result_type == AE_QJS_TSTR) vm->result_str = value_str(q, h, &vm->result_str_len);
    }
    // Reading the result afterwards is not JavaScript time.
    if (outer) qjs_set_deadline_ns(q, 0);
    return rc;
}

int quickjs_vm_result_type(void* v) { return v ? ((AeQjsVm*)v)->result_type : AE_QJS_TNIL; }

int64_t quickjs_vm_result_int(void* v) {
    AeQjsVm* vm = (AeQjsVm*)v;
    return vm && vm->result > 0 ? value_int(vm->q, vm->result) : 0;
}

double quickjs_vm_result_double(void* v) {
    AeQjsVm* vm = (AeQjsVm*)v;
    return vm && vm->result > 0 ? value_double(vm->q, vm->result) : 0.0;
}

// A string result, or an error's message; "" for any other type. An Aether
// string with the text's length, the caller's own.
const void* quickjs_vm_result_str(void* v) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm || !vm->result_str) return out_str("", 0);
    return out_str(vm->result_str, vm->result_str_len);
}

int quickjs_vm_result_bool(void* v) {
    AeQjsVm* vm = (AeQjsVm*)v;
    return vm && vm->result > 0 ? value_bool(vm->q, vm->result) : 0;
}

void* quickjs_vm_result_bytes(void* v) {
    AeQjsVm* vm = (AeQjsVm*)v;
    return vm && vm->result > 0 ? value_bytes(vm->q, vm->result) : NULL;
}

// JSON.stringify of the result; "" for none, an error, or a value with no JSON
// form (undefined, a function) or whose toJSON throws. JSON text never holds a
// raw NUL (JSON.stringify writes \u0000), and a lone surrogate is written as
// its \udXXX escape, so this is the lossless way out for a string that has
// either.
const void* quickjs_vm_result_json(void* v) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm || vm->result <= 0) return out_str("", 0);
    const char* j = nz(qjs_to_json(vm->q, vm->result));
    return out_str(j, strlen(j));
}

void quickjs_vm_pop_result(void* v) {
    if (v) clear_result((AeQjsVm*)v);
}

// ---- globals ----------------------------------------------------------------

static void set_global(AeQjsVm* vm, const char* name, int h) {
    if (h <= 0) return;
    int g = qjs_global(vm->q);
    qjs_set(vm->q, g, name, h);
    qjs_release(vm->q, g);
    qjs_release(vm->q, h);
}

static int number_handle(void* q, int64_t value) {
    if (value >= INT32_MIN && value <= INT32_MAX) return qjs_int(q, (int)value);
    // Past 32 bits a JavaScript number is a double: exact up to 2**53.
    return qjs_float(q, (double)value);
}

// The name of a global as a C string, or NULL when it holds a NUL (the engine
// takes it as a C string, and a cut name is a different name).
static const char* global_name(const void* name) {
    size_t n;
    const char* d = host_bytes(name, &n);
    return has_nul(d, n) ? NULL : d;
}

// The globals' setters return "" when they set the value, else why they did
// not, a constant or the engine's own text, valid until the next call.
const char* quickjs_vm_set_global_str(void* v, const void* name, const void* value) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return "no vm";
    const char* nm = global_name(name);
    if (!nm) return REFUSE("the global's name", WHY_NAME);
    size_t n;
    const char* d = host_bytes(value, &n);
    int h = new_string(vm, d, n);
    if (h <= 0) return "the string could not be made (no memory, or the VM's helper is missing)";
    set_global(vm, nm, h);
    return "";
}

const char* quickjs_vm_set_global_int(void* v, const void* name, int64_t value) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return "no vm";
    const char* nm = global_name(name);
    if (!nm) return REFUSE("the global's name", WHY_NAME);
    set_global(vm, nm, number_handle(vm->q, value));
    return "";
}

// A std.list of strings as an array of strings, each with its own length.
const char* quickjs_vm_set_global_strlist(void* v, const void* name, void* items) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return "no vm";
    const char* nm = global_name(name);
    if (!nm) return REFUSE("the global's name", WHY_NAME);
    int arr = qjs_array(vm->q);
    if (arr <= 0) return nz(qjs_error(vm->q));
    int n = items ? list_size(items) : 0;
    for (int i = 0; i < n; i++) {
        void* item = list_get_raw(items, i);
        size_t len;
        const char* d = host_bytes(item, &len);
        int s = new_string(vm, d, len);
        if (s > 0) {
            qjs_set_index(vm->q, arr, i, s);
            qjs_release(vm->q, s);
        }
    }
    set_global(vm, nm, arr);
    return "";
}

// The value JSON text describes (an array, an object, a number, ...): "" when
// set, else the parse error (nothing is set). JSON text with a raw NUL is not
// JSON, and is said so rather than parsed up to the NUL.
const char* quickjs_vm_set_global_json(void* v, const void* name, const void* json) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return "no vm";
    const char* nm = global_name(name);
    if (!nm) return REFUSE("the global's name", WHY_NAME);
    size_t n;
    const char* d = host_bytes(json, &n);
    if (has_nul(d, n)) return REFUSE("the JSON text", WHY_JSON);
    int h = qjs_parse_json(vm->q, d);
    if (h < 0) return qjs_error(vm->q);
    set_global(vm, nm, h);
    return "";
}

// ---- limits -----------------------------------------------------------------

// Per outermost eval, from its start, over everything it runs (callbacks'
// nested evals and the promise jobs included). ms <= 0 removes it. `every` is
// not used: the engine looks at the clock every few thousand operations.
const char* quickjs_vm_set_time_limit(void* v, int64_t ms, int every) {
    (void)every;
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return "no vm";
    vm->time_limit_ns = ms > 0 ? ms * 1000000 : 0;
    return "";
}

// contrib.quickjs has no operation count to hold a script to
// (asks/contrib-quickjs-op-budget-and-exception-handle.md), so this says so
// rather than ignore the budget, as Duktape's bridge does.
const char* quickjs_vm_set_instruction_limit(void* v, int64_t budget, int every) {
    (void)v; (void)budget; (void)every;
    return "instruction limit unavailable: contrib.quickjs counts no operations (it has a deadline and cancel cells): use vm_set_time_limit";
}

// A std.sync cell whose nonzero value stops the running JavaScript, set from
// any thread (null for none). The cell stays the host's: free it after
// vm_free, or after setting null here.
void quickjs_vm_set_cancel_cell(void* v, void* cell) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (vm) qjs_set_cancel_cell(vm->q, cell);
}

// ---- the host's own pointer, for its callbacks -------------------------------

void quickjs_vm_set_user(void* v, void* p) { if (v) ((AeQjsVm*)v)->user = p; }
void* quickjs_vm_user(void* v) { return v ? ((AeQjsVm*)v)->user : NULL; }

// ---- host functions ---------------------------------------------------------

// "app.math.add": parts of at least one character, the last the function.
// A name with a NUL in it is refused, not cut into a shorter one.
int qjsb_name_ok(const void* nv) {
    size_t n;
    const char* name = host_bytes(nv, &n);
    if (n == 0 || has_nul(name, n)) return 0;
    if (name[0] == '.' || name[n - 1] == '.' || strstr(name, "..")) return 0;
    return 1;
}

// The last part of a dotted name (what the function's `name` is).
const char* qjsb_leaf(const char* name) {
    const char* dot = strrchr(nz(name), '.');
    return dot ? dot + 1 : nz(name);
}

// Puts function handle f at the dotted name, making (or reusing) the objects
// on the way: "app.math.add" is globalThis.app.math.add. A part that is there
// and is not an object or a function is refused, not replaced. Releases f.
// 0, or -1.
int qjsb_install(void* v, const char* name, int f) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return -1;
    void* q = vm->q;
    if (f <= 0) return -1;
    if (!qjsb_name_ok(name)) { qjs_release(q, f); return -1; }
    char* path = dup_str(name);
    if (!path) { qjs_release(q, f); return -1; }
    int obj = qjs_global(q);
    int rc = 0;
    char* part = path;
    for (char* dot = strchr(part, '.'); dot; dot = strchr(part, '.')) {
        *dot = '\0';
        int next = qjs_get(q, obj, part);
        const char* t = next > 0 ? qjs_type(q, next) : "";
        if (next > 0 && (!strcmp(t, "undefined") || !strcmp(t, "null"))) {
            qjs_release(q, next);
            next = qjs_object(q);
            if (next > 0 && qjs_set(q, obj, part, next) < 0) { qjs_release(q, next); next = -1; }
        } else if (next > 0 && strcmp(t, "object") != 0 && strcmp(t, "function") != 0) {
            qjs_release(q, next);
            next = -1;
        }
        qjs_release(q, obj);
        if (next <= 0) { rc = -1; obj = 0; break; }
        obj = next;
        part = dot + 1;
    }
    if (rc == 0 && qjs_set(q, obj, part, f) < 0) rc = -1;
    if (obj > 0) qjs_release(q, obj);
    qjs_release(q, f);
    free(path);
    return rc;
}

// An error thrown into the script with a NUL in its message. The engine's
// qjs_throw takes the message as a C string, so a JavaScript function throws
// it (the message made by new_string) and the engine's rethrow brings that
// error back to throw out of the callback. -1 for the error, as qjs_throw.
static int throw_with_nul(AeQjsVm* vm, const char* msg, size_t n) {
    void* q = vm->q;
    if (vm->thrower > 0) {
        int m = new_string(vm, msg, n);
        int arr = m > 0 ? qjs_array(q) : -1;
        if (arr > 0) {
            qjs_set_index(q, arr, 0, m);
            int r = qjs_call(q, vm->thrower, 0, arr);
            qjs_release(q, arr);
            if (r >= 0) qjs_release(q, r);
            else { qjs_release(q, m); return qjs_rethrow(q); }
        }
        if (m > 0) qjs_release(q, m);
    }
    return qjs_throw(q, "Error", "raise_error: the message could not be thrown whole");
}

// Called by the engine (through module.ae's closure) when a script calls a
// registered host function: host_fn(vm) runs with the call's arguments, and
// its last push is the value (when it returns more than 0), or its
// raise_error the Error the script sees. The engine takes the handle
// returned; this_h and args are the engine's.
int qjsb_dispatch(void* v, void* host_fn, int this_h, int args) {
    (void)this_h;
    AeQjsVm* vm = (AeQjsVm*)v;
    AeQjsFrame saved = vm->frame;
    memset(&vm->frame, 0, sizeof vm->frame);
    vm->frame.args = args;
    int rc = ((int (*)(void*))host_fn)((void*)vm);
    int ret;
    if (vm->frame.raise) {
        if (vm->frame.pushed > 0) qjs_release(vm->q, vm->frame.pushed);
        if (has_nul(vm->frame.raise, vm->frame.raise_len))
            ret = throw_with_nul(vm, vm->frame.raise, vm->frame.raise_len);
        else
            ret = qjs_throw(vm->q, "Error", vm->frame.raise);
    } else if (rc > 0 && vm->frame.pushed > 0) {
        ret = vm->frame.pushed;   // the engine takes it
    } else {
        if (vm->frame.pushed > 0) qjs_release(vm->q, vm->frame.pushed);
        ret = 0;                  // undefined
    }
    free(vm->frame.raise);
    vm->frame = saved;
    return ret;
}

// ---- the callback side ------------------------------------------------------
// Arguments are 1..arg_count(vm), read like results.

static int arg_handle(AeQjsVm* vm, int i) {
    if (!vm || vm->frame.args <= 0 || i < 1 || i > qjs_length(vm->q, vm->frame.args)) return 0;
    int h = qjs_get_index(vm->q, vm->frame.args, i - 1);
    return h > 0 ? h : 0;
}

int quickjs_arg_count(void* v) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm || vm->frame.args <= 0) return 0;
    int n = qjs_length(vm->q, vm->frame.args);
    return n > 0 ? n : 0;
}

int quickjs_arg_type(void* v, int i) {
    AeQjsVm* vm = (AeQjsVm*)v;
    int h = arg_handle(vm, i);
    if (!h) return AE_QJS_TNIL;
    int t = classify(vm->q, h);
    qjs_release(vm->q, h);
    return t;
}

// A string argument's text, "" for any other type (a number is not converted):
// an Aether string with the text's length, the caller's own.
const void* quickjs_arg_str(void* v, int i) {
    AeQjsVm* vm = (AeQjsVm*)v;
    int h = arg_handle(vm, i);
    if (!h) return out_str("", 0);
    size_t n = 0;
    char* s = value_str(vm->q, h, &n);
    qjs_release(vm->q, h);
    void* r = out_str(s ? s : "", s ? n : 0);
    free(s);
    return r;
}

int64_t quickjs_arg_int(void* v, int i) {
    AeQjsVm* vm = (AeQjsVm*)v;
    int h = arg_handle(vm, i);
    if (!h) return 0;
    int64_t n = value_int(vm->q, h);
    qjs_release(vm->q, h);
    return n;
}

double quickjs_arg_double(void* v, int i) {
    AeQjsVm* vm = (AeQjsVm*)v;
    int h = arg_handle(vm, i);
    if (!h) return 0.0;
    double d = value_double(vm->q, h);
    qjs_release(vm->q, h);
    return d;
}

// 1 only for true.
int quickjs_arg_bool(void* v, int i) {
    AeQjsVm* vm = (AeQjsVm*)v;
    int h = arg_handle(vm, i);
    if (!h) return 0;
    int b = value_bool(vm->q, h);
    qjs_release(vm->q, h);
    return b;
}

// A Uint8Array argument as a new std.bytes handle (the caller frees it);
// null for anything else.
void* quickjs_arg_bytes(void* v, int i) {
    AeQjsVm* vm = (AeQjsVm*)v;
    int h = arg_handle(vm, i);
    if (!h) return NULL;
    void* b = value_bytes(vm->q, h);
    qjs_release(vm->q, h);
    return b;
}

// JSON.stringify of the argument; "" for one with no JSON form. As
// vm_result_json: no raw NUL, a lone surrogate as an escape.
const void* quickjs_arg_json(void* v, int i) {
    AeQjsVm* vm = (AeQjsVm*)v;
    int h = arg_handle(vm, i);
    if (!h) return out_str("", 0);
    const char* j = nz(qjs_to_json(vm->q, h));
    void* r = out_str(j, strlen(j));
    qjs_release(vm->q, h);
    return r;
}

static void push(AeQjsVm* vm, int h) {
    if (vm->frame.pushed > 0) qjs_release(vm->q, vm->frame.pushed);
    vm->frame.pushed = h > 0 ? h : 0;
}

void quickjs_push_int(void* v, int64_t n) { AeQjsVm* vm = (AeQjsVm*)v; if (vm) push(vm, number_handle(vm->q, n)); }
// A string, its length and all: a NUL in it crosses.
void quickjs_push_str(void* v, const void* s) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return;
    size_t n;
    const char* d = host_bytes(s, &n);
    push(vm, new_string(vm, d, n));
}
void quickjs_push_bool(void* v, int b) { AeQjsVm* vm = (AeQjsVm*)v; if (vm) push(vm, qjs_bool(vm->q, b != 0)); }
void quickjs_push_nil(void* v) { AeQjsVm* vm = (AeQjsVm*)v; if (vm) push(vm, qjs_null(vm->q)); }
void quickjs_push_double(void* v, double d) { AeQjsVm* vm = (AeQjsVm*)v; if (vm) push(vm, qjs_float(vm->q, d)); }

// `b` is a std.bytes handle; the bytes are copied into a new Uint8Array,
// embedded zeros and all. A null handle pushes null.
void quickjs_push_bytes(void* v, void* b) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return;
    if (!b) { push(vm, qjs_null(vm->q)); return; }
    int n = aether_bytes_length((AetherBytes*)b);
    push(vm, qjs_new_uint8array(vm->q, n > 0 ? aether_bytes_data((AetherBytes*)b) : NULL, n > 0 ? n : 0));
}

static int raise_text(AeQjsVm* vm, const char* msg, size_t n) {
    free(vm->frame.raise);
    vm->frame.raise = (char*)malloc(n + 1);
    if (vm->frame.raise) { memcpy(vm->frame.raise, msg, n); vm->frame.raise[n] = '\0'; vm->frame.raise_len = n; }
    return AE_QJS_RAISE;
}

// The value JSON text describes: 0 when pushed, -1 (nothing pushed) when the
// text is not JSON. A raw NUL is not JSON either, and the callback's script
// also gets an Error that says so (the host may not look at the -1).
int quickjs_push_json(void* v, const void* json) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return -1;
    size_t n;
    const char* d = host_bytes(json, &n);
    if (has_nul(d, n)) {
        raise_text(vm, REFUSE("push_json", WHY_JSON), strlen(REFUSE("push_json", WHY_JSON)));
        return -1;
    }
    int h = qjs_parse_json(vm->q, d);
    if (h < 0) return -1;
    push(vm, h);
    return 0;
}

// `return quickjs.raise_error(vm, msg)`: the script sees Error(msg), which it
// can catch, after the callback has returned (its defers run). A push before
// it is dropped.
int quickjs_raise_error(void* v, const void* msg) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return AE_QJS_RAISE;
    size_t n;
    const char* d = host_bytes(msg, &n);
    if (n == 0) return raise_text(vm, "error", 5);
    return raise_text(vm, d, n);
}

// ---- the bridge's own host functions ----------------------------------------

// print(...) and console.log(...): the arguments String()'d, space-joined,
// and a newline, on stdout. Nothing is checked: it writes to the process's
// own stdout, as Duktape's print does. In the contained child they are SAY.
static int print_cb(void* v) {
    AeQjsVm* vm = (AeQjsVm*)v;
    int n = quickjs_arg_count(vm);
    for (int i = 1; i <= n; i++) {
        int h = arg_handle(vm, i);
        if (i > 1) fputc(' ', stdout);
        if (h) {
            int len = qjs_to_string(vm->q, h);
            if (len > 0) fwrite(qjs_scratch(vm->q), 1, (size_t)len, stdout);
            qjs_release(vm->q, h);
        }
    }
    fputc('\n', stdout);
    fflush(stdout);
    return 0;
}

// Not in aether_shared_map.h; the Duktape host declares it the same way.
extern void aether_shared_map_freeze_inputs_by_token(uint64_t token);

// The shared map is Aether's runtime's (aether_shared_map_*), and it keeps C
// strings: a key or a value with a NUL cannot be stored or read whole there.
// So the script's aether_map_get and aether_map_put refuse one, with an Error
// that names the reason, instead of cutting it (asks/shared-map-nul.md).

// A string argument as a malloc'd C string and its length ("" for any other
// type, as arg_str); the caller frees it.
static char* arg_copy(AeQjsVm* vm, int i, size_t* n) {
    int h = arg_handle(vm, i);
    *n = 0;
    if (h) {
        char* s = value_str(vm->q, h, n);
        qjs_release(vm->q, h);
        if (s) return s;
    }
    *n = 0;
    return dup_str("");
}

// aether_map_get(key): the value, or undefined.
static int map_get_cb(void* v) {
    AeQjsVm* vm = (AeQjsVm*)v;
    size_t kn;
    char* key = arg_copy(vm, 1, &kn);
    if (!key) return 0;
    if (has_nul(key, kn)) {
        free(key);
        return raise_text(vm, REFUSE("aether_map_get: the key", WHY_MAP), strlen(REFUSE("aether_map_get: the key", WHY_MAP)));
    }
    const char* val = aether_shared_map_get_by_token(vm->map_token, key);
    free(key);
    if (!val) return 0;
    push(vm, new_string(vm, val, strlen(val)));
    return 1;
}

// aether_map_put(key, value): a frozen input is not written (the map says no).
static int map_put_cb(void* v) {
    AeQjsVm* vm = (AeQjsVm*)v;
    size_t kn, vn;
    char* key = arg_copy(vm, 1, &kn);
    char* val = arg_copy(vm, 2, &vn);
    int rc = 0;
    if (!key || !val) {
        // out of memory: nothing is stored
    } else if (has_nul(key, kn)) {
        rc = raise_text(vm, REFUSE("aether_map_put: the key", WHY_MAP), strlen(REFUSE("aether_map_put: the key", WHY_MAP)));
    } else if (has_nul(val, vn)) {
        rc = raise_text(vm, REFUSE("aether_map_put: the value", WHY_MAP), strlen(REFUSE("aether_map_put: the value", WHY_MAP)));
    } else {
        aether_shared_map_put_by_token(vm->map_token, key, val);
    }
    free(key);
    free(val);
    return rc;
}

void* qjsb_print_fn(void) { return (void*)print_cb; }
void* qjsb_map_get_fn(void) { return (void*)map_get_cb; }
void* qjsb_map_put_fn(void) { return (void*)map_put_cb; }

// The map run_sandboxed_with_map's aether_map_get and aether_map_put use,
// its inputs frozen first. The token is this VM's, not the process's.
void qjsb_set_map(void* v, uint64_t token) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return;
    aether_shared_map_freeze_inputs_by_token(token);
    vm->map_token = token;
}

// The grants as a level for the length of a run: 0, or -1 when the stack of
// levels is full (nothing pushed).
int qjsb_push_grants(void* perms) {
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;
    aether_sandbox_push(perms);
    return 0;
}

void qjsb_pop_grants(void) { aether_sandbox_pop(); }

// run's failure line: "[js] " and the error with its stack, on stderr.
void qjsb_report(void* v) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return;
    const char* t = vm->result_full ? vm->result_full : vm->result_str;
    size_t n = vm->result_full ? vm->result_full_len : vm->result_str_len;
    fputs("[js] ", stderr);
    if (t) fwrite(t, 1, n, stderr);
    fputc('\n', stderr);
}

int quickjs_init(void) { return 0; }
void quickjs_finalize(void) {}
