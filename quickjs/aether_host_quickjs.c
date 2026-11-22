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
// Strings and NUL. JavaScript strings hold a NUL like any other character,
// and contrib.quickjs (Aether 0.935.0) takes every string by its length and
// hands every string out with it: script text, names, JSON text, messages and
// the error's text all carry a NUL whole. So does the shared map (keys and
// values by their length since the same release). The one string the engine
// still refuses with a NUL is a script's file name, which no host here gives
// it. What the engine hands out is valid UTF-8: a lone surrogate is U+FFFD
// (the module's conversion), and JSON keeps it exactly, as \udXXX.
//
// Every string given to the engine is an Aether string (its @aether string
// parameters): one the host gave is passed on as it came, one made here is
// made with string_new_with_length and released after the call. Never a raw
// char*, whose bytes the engine's helpers would read as a possible header.
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
extern int qjs_eval(void* q, const void* code, const void* filename, int flags);
extern int qjs_run_jobs(void* q);
extern const void* qjs_error_data(void* q);
extern int qjs_error_len(void* q);
extern int qjs_last_exception(void* q);
extern void qjs_set_poll_budget(void* q, int64_t polls);
extern int qjs_throw(void* q, const void* kind, const void* msg);
extern void qjs_release(void* q, int h);
extern int qjs_global(void* q);
extern int qjs_null(void* q);
extern int qjs_bool(void* q, int b);
extern int qjs_int(void* q, int n);
extern int qjs_float(void* q, double d);
extern int qjs_string(void* q, const void* s);
extern int qjs_object(void* q);
extern int qjs_array(void* q);
extern int qjs_parse_json(void* q, const void* text);
extern int qjs_to_json(void* q, int h);
extern int qjs_to_string(void* q, int h);
extern const void* qjs_scratch(void* q);
extern double qjs_to_float(void* q, int h);
extern int qjs_to_bool(void* q, int h);
extern const char* qjs_type(void* q, int h);
extern int qjs_get(void* q, int obj, const void* name);
extern int qjs_set(void* q, int obj, const void* name, int v);
extern int qjs_get_index(void* q, int obj, int i);
extern int qjs_set_index(void* q, int obj, int i, int v);
extern int qjs_length(void* q, int obj);
extern int qjs_u8_length(void* q, int h);
extern const void* qjs_u8_data(void* q, int h);
extern int qjs_new_uint8array(void* q, const void* data, int n);
// The engine's interrupt reasons (contrib.quickjs INTERRUPTED_*).
#define QJS_INTERRUPT_DEADLINE 1
#define QJS_INTERRUPT_CANCEL 2
#define QJS_INTERRUPT_STOP 3
#define QJS_INTERRUPT_BUDGET 4
// The engine polls its interrupt handler once per this many counted steps
// (quickjs-ng's JS_INTERRUPT_COUNTER_INIT): a poll budget is in these units.
#define QJS_STEPS_PER_POLL 10000

// std.list and std.string, for vm_set_global_strlist and the strings that
// cross (an Aether string is its bytes and its length; a plain char* reads the
// same through these).
extern int list_size(void*);
extern void* list_get_raw(void*, int);
extern const char* aether_string_data(const void*);
extern size_t aether_string_length(const void*);
extern void* string_new_with_length(const char* data, size_t length);
extern void string_release(const void* s);

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
    AeQjsFrame frame;
    int64_t time_limit_ns; // per outermost eval; 0 for none
    int64_t poll_budget;   // interrupt polls per outermost eval; 0 for none
    int depth;             // evals running on this VM (nested ones count)
    void* user;            // the host's, for its callbacks (vm_user)
    uint64_t map_token;    // run_sandboxed_with_map's map, for its callbacks
} AeQjsVm;

// A string the host gave (an Aether string, or a plain char*): its bytes and
// its length, NULL read as "".
static const char* host_bytes(const void* v, size_t* n) {
    const char* d = aether_string_data(v);
    if (!d) { *n = 0; return ""; }
    *n = aether_string_length(v);
    return d;
}

// An Aether string made from bytes and a length, for the host (NUL and all).
static void* out_str(const char* d, size_t n) {
    return string_new_with_length(d ? d : "", d ? n : 0);
}

// An Aether string of bytes and a length, for an engine parameter: the
// caller releases it (string_release) after the call. NULL without memory.
static void* eng_str(const char* d, size_t n) { return string_new_with_length(d, n); }

// The engine's string from bytes and a length (-1 when it cannot be made).
static int new_string(void* q, const char* d, size_t n) {
    void* s = eng_str(d, n);
    if (!s) return -1;
    int h = qjs_string(q, s);
    string_release(s);
    return h;
}

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

static char* text_of(void* q, int h, size_t* len);

// A string's UTF-8 text and length (a NUL kept: the engine counts bytes), as
// a new malloc'd copy; NULL when h is no string. The engine hands out valid
// UTF-8: a lone surrogate is U+FFFD, a character beyond the BMP its four-byte
// form, not CESU-8.
static char* value_str(void* q, int h, size_t* len) {
    if (strcmp(qjs_type(q, h), "string") != 0) return NULL;
    return text_of(q, h, len);
}

// String(h) as a new malloc'd copy and its length (a NUL kept), whatever h
// is; NULL when the conversion throws or there is no memory.
static char* text_of(void* q, int h, size_t* len) {
    int n = qjs_to_string(q, h);
    if (n < 0) return NULL;
    char* s = (char*)malloc((size_t)n + 1);
    if (!s) return NULL;
    memcpy(s, qjs_scratch(q), (size_t)n);
    s[n] = '\0';
    if (len) *len = (size_t)n;
    return s;
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

// A copy of bytes and a length, NUL-terminated after them (NUL inside kept).
static char* copy_bytes(const char* d, size_t n) {
    char* c = (char*)malloc(n + 1);
    if (!c) return NULL;
    if (n) memcpy(c, d, n);
    c[n] = '\0';
    return c;
}

static void set_error_text(AeQjsVm* vm, const char* text, size_t n) {
    vm->result_type = AE_QJS_TERR;
    vm->result_str = copy_bytes(text, n);
    vm->result_str_len = vm->result_str ? n : 0;
    vm->result_full = copy_bytes(text, n);
    vm->result_full_len = vm->result_full ? n : 0;
}

// The error the last failed eval left. result_full is the engine's text,
// String(e), a newline and the stack, every byte (qjs_error_data and
// qjs_error_len). result_str is String(e) alone, read from the thrown value
// itself (qjs_last_exception), so a message holding a newline or a NUL is
// kept whole; when there is no value (a host refusal, a full heap) or its
// String(e) throws, it is the engine's whole text. An interrupt says which
// limit stopped JavaScript instead.
static void set_error(AeQjsVm* vm, int interrupted) {
    void* q = vm->q;
    // Read before anything below runs JavaScript (and so may fail again).
    vm->result_full = copy_bytes((const char*)qjs_error_data(q), (size_t)qjs_error_len(q));
    vm->result_full_len = vm->result_full ? (size_t)qjs_error_len(q) : 0;
    vm->result_type = AE_QJS_TERR;
    const char* why = NULL;
    if (interrupted == QJS_INTERRUPT_DEADLINE) why = "InternalError: interrupted: time limit exceeded";
    else if (interrupted == QJS_INTERRUPT_CANCEL) why = "InternalError: interrupted: cancelled";
    else if (interrupted == QJS_INTERRUPT_BUDGET) why = "InternalError: interrupted: instruction limit exceeded";
    else if (interrupted == QJS_INTERRUPT_STOP) why = "InternalError: interrupted: stopped";
    if (why) {
        vm->result_str = copy_bytes(why, strlen(why));
        vm->result_str_len = vm->result_str ? strlen(why) : 0;
        return;
    }
    int e = qjs_last_exception(q);
    size_t n = 0;
    char* msg = e > 0 ? text_of(q, e, &n) : NULL;
    if (e > 0) qjs_release(q, e);
    if (!msg && vm->result_full) { msg = copy_bytes(vm->result_full, vm->result_full_len); n = vm->result_full_len; }
    vm->result_str = msg;
    vm->result_str_len = msg ? n : 0;
}

// Runs code at global level, then the promise jobs it queued. 0 with the
// script's completion value as the result (statements give undefined), or
// -1 with the error as the result (QJS_TERR, its message as the string). A
// time limit and an instruction limit apply to the outermost eval and
// everything it runs, a nested eval (from a callback) included, which neither
// renews nor removes them. The script is every byte of code: a NUL inside a
// string literal is a character, one elsewhere a SyntaxError.
int quickjs_vm_eval(void* v, const void* code) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm || !code) return -1;
    void* q = vm->q;
    clear_result(vm);
    void* filename = eng_str("<eval>", 6);
    if (!filename) {
        set_error_text(vm, "InternalError: out of memory", 28);
        return -1;
    }
    int outer = vm->depth == 0;
    if (outer) {
        qjs_clear_interrupted(q);
        qjs_set_deadline_ns(q, vm->time_limit_ns > 0 ? qjs_now_ns() + vm->time_limit_ns : 0);
        qjs_set_poll_budget(q, vm->poll_budget > 0 ? vm->poll_budget : -1);
    }
    vm->depth++;
    int h = qjs_eval(q, code, filename, 0);
    string_release(filename);
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
    // Reading the result afterwards is not JavaScript time, or steps.
    if (outer) { qjs_set_deadline_ns(q, 0); qjs_set_poll_budget(q, -1); }
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
    int n = qjs_to_json(vm->q, vm->result);
    return n < 0 ? out_str("", 0) : out_str((const char*)qjs_scratch(vm->q), (size_t)n);
}

void quickjs_vm_pop_result(void* v) {
    if (v) clear_result((AeQjsVm*)v);
}

// ---- globals ----------------------------------------------------------------

// name is the host's Aether string, every byte the key (a NUL included).
static void set_global(AeQjsVm* vm, const void* name, int h) {
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

// The globals' setters return "" when they set the value, else why they did
// not, a constant or the engine's own text, valid until the next call. A
// global's name is every byte of the string, a NUL included.
const char* quickjs_vm_set_global_str(void* v, const void* name, const void* value) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return "no vm";
    int h = qjs_string(vm->q, value);
    if (h <= 0) return (const char*)qjs_error_data(vm->q);
    set_global(vm, name, h);
    return "";
}

const char* quickjs_vm_set_global_int(void* v, const void* name, int64_t value) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return "no vm";
    set_global(vm, name, number_handle(vm->q, value));
    return "";
}

// A std.list of strings as an array of strings, each with its own length.
const char* quickjs_vm_set_global_strlist(void* v, const void* name, void* items) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return "no vm";
    int arr = qjs_array(vm->q);
    if (arr <= 0) return (const char*)qjs_error_data(vm->q);
    int n = items ? list_size(items) : 0;
    for (int i = 0; i < n; i++) {
        int s = qjs_string(vm->q, list_get_raw(items, i));
        if (s > 0) {
            qjs_set_index(vm->q, arr, i, s);
            qjs_release(vm->q, s);
        }
    }
    set_global(vm, name, arr);
    return "";
}

// The value JSON text describes (an array, an object, a number, ...): "" when
// set, else the parse error (nothing is set). Every byte is parsed: a raw NUL
// is a SyntaxError, as any stray character is, never the end of the text.
const char* quickjs_vm_set_global_json(void* v, const void* name, const void* json) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return "no vm";
    int h = qjs_parse_json(vm->q, json);
    if (h < 0) return (const char*)qjs_error_data(vm->q);
    set_global(vm, name, h);
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

// Per outermost eval, as the time limit: stops JavaScript after `budget`
// counted steps, budget <= 0 removes it. The engine counts in its interrupt
// polls (contrib.quickjs's set_poll_budget), one per 10,000 steps (a
// function call, a jump the interpreter takes or tests, an element step of a
// built-in that iterates, 10,000 regexp backtracking steps per poll), so the
// budget is ceil(budget / 10000) polls, and JavaScript stops after between
// that many polls less one and that many, times 10,000 steps: the granularity
// is 10,000. `every` is not used: the engine's poll interval is fixed.
const char* quickjs_vm_set_instruction_limit(void* v, int64_t budget, int every) {
    (void)every;
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return "no vm";
    vm->poll_budget = budget > 0 ? (budget + QJS_STEPS_PER_POLL - 1) / QJS_STEPS_PER_POLL : 0;
    return "";
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

// "app.math.add": parts of at least one byte, the last the function. A part
// is every byte between the dots, a NUL included (the engine keys a property
// by its whole name).
int qjsb_name_ok(const void* nv) {
    size_t n;
    const char* name = host_bytes(nv, &n);
    if (n == 0 || name[0] == '.' || name[n - 1] == '.') return 0;
    for (size_t i = 0; i + 1 < n; i++) if (name[i] == '.' && name[i + 1] == '.') return 0;
    return 1;
}

// The last part of a dotted name (what the function's `name` is), as a new
// Aether string, the caller's.
const void* qjsb_leaf(const void* nv) {
    size_t n;
    const char* name = host_bytes(nv, &n);
    size_t start = n;
    while (start > 0 && name[start - 1] != '.') start--;
    return out_str(name + start, n - start);
}

// Puts function handle f at the dotted name, making (or reusing) the objects
// on the way: "app.math.add" is globalThis.app.math.add. A part that is there
// and is not an object or a function is refused, not replaced. Releases f.
// 0, or -1.
int qjsb_install(void* v, const void* nv, int f) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return -1;
    void* q = vm->q;
    if (f <= 0) return -1;
    if (!qjsb_name_ok(nv)) { qjs_release(q, f); return -1; }
    size_t n;
    const char* name = host_bytes(nv, &n);
    int obj = qjs_global(q);
    int rc = 0;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (name[i] != '.') continue;
        void* part = eng_str(name + start, i - start);
        int next = part ? qjs_get(q, obj, part) : -1;
        const char* t = next > 0 ? qjs_type(q, next) : "";
        if (next > 0 && (!strcmp(t, "undefined") || !strcmp(t, "null"))) {
            qjs_release(q, next);
            next = qjs_object(q);
            if (next > 0 && qjs_set(q, obj, part, next) < 0) { qjs_release(q, next); next = -1; }
        } else if (next > 0 && strcmp(t, "object") != 0 && strcmp(t, "function") != 0) {
            qjs_release(q, next);
            next = -1;
        }
        if (part) string_release(part);
        qjs_release(q, obj);
        if (next <= 0) { rc = -1; obj = 0; break; }
        obj = next;
        start = i + 1;
    }
    if (rc == 0) {
        void* leaf = eng_str(name + start, n - start);
        if (!leaf || qjs_set(q, obj, leaf, f) < 0) rc = -1;
        if (leaf) string_release(leaf);
    }
    if (obj > 0) qjs_release(q, obj);
    qjs_release(q, f);
    return rc;
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
        // The message by its length, a NUL and all.
        void* kind = eng_str("Error", 5);
        void* msg = eng_str(vm->frame.raise, vm->frame.raise_len);
        ret = kind && msg ? qjs_throw(vm->q, kind, msg) : -1;
        if (kind) string_release(kind);
        if (msg) string_release(msg);
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
    int n = qjs_to_json(vm->q, h);
    void* r = n < 0 ? out_str("", 0) : out_str((const char*)qjs_scratch(vm->q), (size_t)n);
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
    push(vm, qjs_string(vm->q, s));
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
// text is not JSON (every byte is parsed: a raw NUL is a SyntaxError).
int quickjs_push_json(void* v, const void* json) {
    AeQjsVm* vm = (AeQjsVm*)v;
    if (!vm) return -1;
    int h = qjs_parse_json(vm->q, json);
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

// The shared map is Aether's runtime's (aether_shared_map_*), by token: keys
// and values by their length, a NUL in either kept, in and out.

// A string argument as a malloc'd copy and its length ("" for any other type,
// as arg_str); the caller frees it.
static char* arg_copy(AeQjsVm* vm, int i, size_t* n) {
    int h = arg_handle(vm, i);
    *n = 0;
    if (h) {
        char* s = value_str(vm->q, h, n);
        qjs_release(vm->q, h);
        if (s) return s;
    }
    *n = 0;
    return copy_bytes("", 0);
}

// aether_map_get(key): the value, or undefined.
static int map_get_cb(void* v) {
    AeQjsVm* vm = (AeQjsVm*)v;
    size_t kn, vn = 0;
    char* key = arg_copy(vm, 1, &kn);
    if (!key) return 0;
    const char* val = aether_shared_map_get_by_token(vm->map_token, key, kn, &vn);
    free(key);
    if (!val) return 0;
    push(vm, new_string(vm->q, val, vn));
    return 1;
}

// aether_map_put(key, value): a frozen input is not written (the map says no).
static int map_put_cb(void* v) {
    AeQjsVm* vm = (AeQjsVm*)v;
    size_t kn, vn;
    char* key = arg_copy(vm, 1, &kn);
    char* val = arg_copy(vm, 2, &vn);
    if (key && val) aether_shared_map_put_by_token(vm->map_token, key, kn, val, vn);
    free(key);
    free(val);
    return 0;
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
