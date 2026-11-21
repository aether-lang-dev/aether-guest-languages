// aether_host_tcl.c — Embedded Tcl Language Host Module
//
// Embeds libtcl in the Aether process. When run_sandboxed is called,
// pushes its grants as a level of the calling thread's (#2955), so Tcl's libc calls (open,
// getenv, socket, exec) are intercepted and checked against the
// grant list.
//
// LOADING MODEL: dlopen, not -ltcl. Mirrors contrib/host/ruby,
// perl, js — every Tcl C-API symbol resolved via dlsym
// at first call. The bridge .a has NO unresolved Tcl_* symbols at
// link time, so end-user binaries have no DT_NEEDED libtcl and are
// ABI-portable across deploy-host Tcl minor versions (8.5 / 8.6 / 9.0 / 9.1).
//
// Tcl's C API is MOSTLY non-macro — most `Tcl_*` symbols are real
// exported functions, so the bridge is largely a plain dlsym table.
// The exceptions (since Tcl 9.0): `Tcl_GetStringResult`, `Tcl_GetString`
// and `Tcl_Eval` became function-like macros over `Tcl_GetStringFromObj`
// and `Tcl_EvalEx`, and are no longer exported. We dlsym the stable LCD
// exports (`Tcl_GetStringFromObj`, `Tcl_GetObjResult`, `Tcl_EvalEx`) and
// recompose those three accessors — see the helpers below.

#include "aether_host_tcl.h"
// The installed runtime's headers (include/aether/runtime), which `ae build`
// puts on the include path of every C file it compiles into a program.
// libaether is always built with the sandbox; aether's Makefile says so to
// its own C with -DAETHER_HAS_SANDBOX, which a program's build does not pass.
#ifndef AETHER_HAS_SANDBOX
#define AETHER_HAS_SANDBOX 1
#endif
#include "aether_sandbox.h"
#include "aether_shared_map.h"

// In aether's tree this was defined by `make contrib` when pkg-config found
// a Tcl dev package. Here the declarations travel with the bridge
// (tcl_abi.h), so it always builds; libtcl is still found only at run time.
#ifndef AETHER_HAS_TCL
#define AETHER_HAS_TCL 1
#endif

#if AETHER_HAS_TCL
#include "tcl_abi.h"
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- libtcl dlopen table ----------------------------------------------------

static void* libtcl_handle = NULL;

static struct {
    // Lifecycle.
    void         (*Tcl_FindExecutable)(const char* argv0);
    Tcl_Interp*  (*Tcl_CreateInterp)(void);
    int          (*Tcl_Init)(Tcl_Interp* interp);
    void         (*Tcl_DeleteInterp)(Tcl_Interp* interp);
    void         (*Tcl_Finalize)(void);
    // Eval + result extraction. We resolve the LCD real exports
    // (present in both 8.6 and 9.0) rather than the now-macro
    // Tcl_Eval / Tcl_GetStringResult / Tcl_GetString, then compose them
    // in the tcl_eval() / tcl_string_result() / tcl_obj_string() helpers
    // below. The last arg of Tcl_GetStringFromObj is int* on 8.6 and
    // Tcl_Size* on 9.0; we only ever pass NULL, so type it void* to stay
    // width-agnostic.
    int          (*Tcl_EvalEx)(Tcl_Interp* interp, const char* script,
                               Tcl_Size numBytes, int flags);
    Tcl_Obj*     (*Tcl_GetObjResult)(Tcl_Interp* interp);
    void         (*Tcl_GetVersion)(int* major, int* minor, int* patch, int* type);
    const char*  (*Tcl_GetStringFromObj)(Tcl_Obj* objPtr, void* lengthPtr);
    // Object/string helpers (for the shared-map command callbacks).
    Tcl_Obj*     (*Tcl_NewStringObj)(const char* bytes, Tcl_Size length);
    void         (*Tcl_SetObjResult)(Tcl_Interp* interp, Tcl_Obj* resultObjPtr);
    void         (*Tcl_WrongNumArgs)(Tcl_Interp* interp, Tcl_Size objc,
                                     Tcl_Obj* const objv[], const char* message);
    Tcl_Command  (*Tcl_CreateObjCommand)(Tcl_Interp* interp, const char* cmdName,
                                         Tcl_ObjCmdProc* proc, ClientData clientData,
                                         Tcl_CmdDeleteProc* deleteProc);
    // The bidirectional tier (#904). All real exports in 8.6 and 9.0. The
    // reference counts go through Tcl_DbIncrRefCount / Tcl_DbDecrRefCount,
    // the functions behind the Tcl_IncrRefCount macros, because those macros
    // write the refCount field, whose width 9.0 changed.
    void         (*Tcl_DbIncrRefCount)(Tcl_Obj* objPtr, const char* file, int line);
    void         (*Tcl_DbDecrRefCount)(Tcl_Obj* objPtr, const char* file, int line);
    Tcl_Obj*     (*Tcl_NewWideIntObj)(Tcl_WideInt wideValue);
    Tcl_Obj*     (*Tcl_NewDoubleObj)(double doubleValue);
    Tcl_Obj*     (*Tcl_NewListObj)(Tcl_Size objc, Tcl_Obj* const objv[]);
    int          (*Tcl_ListObjAppendElement)(Tcl_Interp* interp, Tcl_Obj* listPtr,
                                             Tcl_Obj* objPtr);
    Tcl_Obj*     (*Tcl_SetVar2Ex)(Tcl_Interp* interp, const char* part1,
                                  const char* part2, Tcl_Obj* newValuePtr, int flags);
    int          (*Tcl_GetWideIntFromObj)(Tcl_Interp* interp, Tcl_Obj* objPtr,
                                          Tcl_WideInt* widePtr);
    int          (*Tcl_GetDoubleFromObj)(Tcl_Interp* interp, Tcl_Obj* objPtr,
                                         double* doublePtr);
    int          (*Tcl_GetBooleanFromObj)(Tcl_Interp* interp, Tcl_Obj* objPtr,
                                          int* boolPtr);
    void         (*Tcl_ResetResult)(Tcl_Interp* interp);
    int          (*Tcl_EvalObjEx)(Tcl_Interp* interp, Tcl_Obj* objPtr, int flags);
    Tcl_Obj*     (*Tcl_GetReturnOptions)(Tcl_Interp* interp, int result);
    int          (*Tcl_DictObjGet)(Tcl_Interp* interp, Tcl_Obj* dictPtr,
                                   Tcl_Obj* keyPtr, Tcl_Obj** valuePtrPtr);
    int          (*Tcl_GetIntFromObj)(Tcl_Interp* interp, Tcl_Obj* objPtr, int* intPtr);
    // Execution guards: interpreter resource limits.
    // int on 8.x, Tcl_Size on 9; Tcl_Size here (tcl_abi.h), values below INT_MAX.
    void         (*Tcl_LimitSetCommands)(Tcl_Interp* interp, Tcl_Size commandLimit);
    void         (*Tcl_LimitSetTime)(Tcl_Interp* interp, Tcl_Time* timeLimitPtr);
    void         (*Tcl_LimitSetGranularity)(Tcl_Interp* interp, int type,
                                            int granularity);
    void         (*Tcl_LimitTypeSet)(Tcl_Interp* interp, int type);
    void         (*Tcl_LimitTypeReset)(Tcl_Interp* interp, int type);
    void         (*Tcl_GetTime)(Tcl_Time* timeBuf);
    // Only to find out how this Tcl stores a character beyond the BMP: see
    // ae_tcl_utf4_native. int on 8.x, Tcl_Size on 9; the value is 3 or 4.
    int          (*Tcl_UniCharToUtf)(int ch, char* buf);
    // Optional: Tcl 9's zipfs hook, which tclsh calls first in main(). Absent
    // in 8.x (no zipfs there) and so never required: see tcl_process_init().
    const char*  (*TclZipfs_AppHook)(int* argcPtr, char*** argvPtr);
} g_tcl;
static int resolve_tcl_symbols(void* h) {
#define RESOLVE(field, sym) do {                                       \
        *(void**)(&g_tcl.field) = dlsym(h, sym);                       \
        if (!g_tcl.field) {                                            \
            fprintf(stderr,                                            \
                "aether host_tcl: libtcl missing symbol %s\n", sym);   \
            return -1;                                                 \
        }                                                              \
    } while (0)

    RESOLVE(Tcl_FindExecutable,    "Tcl_FindExecutable");
    RESOLVE(Tcl_CreateInterp,      "Tcl_CreateInterp");
    RESOLVE(Tcl_Init,              "Tcl_Init");
    RESOLVE(Tcl_DeleteInterp,      "Tcl_DeleteInterp");
    RESOLVE(Tcl_Finalize,          "Tcl_Finalize");
    RESOLVE(Tcl_EvalEx,            "Tcl_EvalEx");
    RESOLVE(Tcl_GetObjResult,      "Tcl_GetObjResult");
    RESOLVE(Tcl_GetVersion,        "Tcl_GetVersion");
    RESOLVE(Tcl_GetStringFromObj,  "Tcl_GetStringFromObj");
    RESOLVE(Tcl_NewStringObj,      "Tcl_NewStringObj");
    RESOLVE(Tcl_SetObjResult,      "Tcl_SetObjResult");
    RESOLVE(Tcl_WrongNumArgs,      "Tcl_WrongNumArgs");
    RESOLVE(Tcl_CreateObjCommand,  "Tcl_CreateObjCommand");
    RESOLVE(Tcl_DbIncrRefCount,    "Tcl_DbIncrRefCount");
    RESOLVE(Tcl_DbDecrRefCount,    "Tcl_DbDecrRefCount");
    RESOLVE(Tcl_NewWideIntObj,     "Tcl_NewWideIntObj");
    RESOLVE(Tcl_NewDoubleObj,      "Tcl_NewDoubleObj");
    RESOLVE(Tcl_NewListObj,        "Tcl_NewListObj");
    RESOLVE(Tcl_ListObjAppendElement, "Tcl_ListObjAppendElement");
    RESOLVE(Tcl_SetVar2Ex,         "Tcl_SetVar2Ex");
    RESOLVE(Tcl_GetWideIntFromObj, "Tcl_GetWideIntFromObj");
    RESOLVE(Tcl_GetDoubleFromObj,  "Tcl_GetDoubleFromObj");
    RESOLVE(Tcl_GetBooleanFromObj, "Tcl_GetBooleanFromObj");
    RESOLVE(Tcl_ResetResult,       "Tcl_ResetResult");
    RESOLVE(Tcl_EvalObjEx,         "Tcl_EvalObjEx");
    RESOLVE(Tcl_GetReturnOptions,  "Tcl_GetReturnOptions");
    RESOLVE(Tcl_DictObjGet,        "Tcl_DictObjGet");
    RESOLVE(Tcl_GetIntFromObj,     "Tcl_GetIntFromObj");
    RESOLVE(Tcl_LimitSetCommands,  "Tcl_LimitSetCommands");
    RESOLVE(Tcl_LimitSetTime,      "Tcl_LimitSetTime");
    RESOLVE(Tcl_LimitSetGranularity, "Tcl_LimitSetGranularity");
    RESOLVE(Tcl_LimitTypeSet,      "Tcl_LimitTypeSet");
    RESOLVE(Tcl_LimitTypeReset,    "Tcl_LimitTypeReset");
    RESOLVE(Tcl_GetTime,           "Tcl_GetTime");
    RESOLVE(Tcl_UniCharToUtf,      "Tcl_UniCharToUtf");
    // Not in 8.5 or 8.6, so not RESOLVE: NULL there is the normal case.
    *(void**)(&g_tcl.TclZipfs_AppHook) = dlsym(h, "TclZipfs_AppHook");
    return 0;
#undef RESOLVE
}

/* Replacement for the Tcl_Eval macro. A -1 length means "script is
 * NUL-terminated, measure it" on both 8.6 and 9.0 (9.0 spells the same
 * value TCL_INDEX_NONE); 0 flags matches what Tcl_Eval always passed. */
static int tcl_eval(Tcl_Interp* interp, const char* script) {
    return g_tcl.Tcl_EvalEx(interp, script, (Tcl_Size)-1, 0);
}

/* Replacements for the Tcl_GetStringResult / Tcl_GetString macros,
 * composed from the two LCD real exports. NULL length pointer = "don't
 * report the length" on both 8.6 (int*) and 9.0 (Tcl_Size*). */
static const char* tcl_string_result(Tcl_Interp* interp) {
    return g_tcl.Tcl_GetStringFromObj(g_tcl.Tcl_GetObjResult(interp), NULL);
}
static const char* tcl_obj_string(Tcl_Obj* obj) {
    return g_tcl.Tcl_GetStringFromObj(obj, NULL);
}

/* ---- strings across the boundary ------------------------------------------
 *
 * What Tcl keeps inside is not UTF-8. A string is "modified UTF-8":
 *   - a NUL is the two bytes C0 80 (Tcl_GetStringFromObj never returns a
 *     zero byte inside a string), which is invalid UTF-8;
 *   - a Tcl built with 16-bit characters (every 8.x by default, TCL_UTF_MAX 3)
 *     stores a character beyond the BMP as a surrogate pair, each half three
 *     bytes (ED A0..AF xx, ED B0..BF xx: CESU-8), also invalid UTF-8;
 *     Tcl 9 stores it as the four UTF-8 bytes.
 * Handing Tcl_GetStringFromObj's bytes to the host as if they were UTF-8 sent
 * the first of these as "C0 80" and the second as six bytes. And the other way,
 * Tcl_NewStringObj on a four-byte UTF-8 sequence puts four bytes into a string
 * that a 16-bit Tcl then reads as four stray characters.
 *
 * So every string that crosses is converted here. A NUL is REFUSED, not carried
 * (the repository's decision: a refusal the caller sees beats a string that is
 * silently wrong), and so is a lone surrogate, which UTF-8 cannot encode. A
 * character beyond the BMP is carried, by joining or splitting the pair. The
 * answer is "this string cannot cross" and a reason, never a changed string.
 */
#define AE_TCL_REFUSE_NUL \
    "an embedded NUL; Tcl stores NUL as the two bytes C0 80, which are not UTF-8, so this bridge refuses it rather than pass on bytes that are wrong"
#define AE_TCL_REFUSE_SURROGATE \
    "an unpaired surrogate, which UTF-8 cannot encode"

enum { AE_STR_OK = 0, AE_STR_NUL = 1, AE_STR_SURROGATE = 2 };

static const char* ae_tcl_reason(int status) {
    return status == AE_STR_NUL ? AE_TCL_REFUSE_NUL : AE_TCL_REFUSE_SURROGATE;
}

/* Whether this Tcl stores a character beyond the BMP as four UTF-8 bytes
 * (Tcl 9, or an 8.x built with TCL_UTF_MAX 4) rather than as a surrogate pair.
 * Found by asking Tcl to encode one, not by its version. */
static int ae_tcl_utf4_native(void) {
    char buf[8];
    return g_tcl.Tcl_UniCharToUtf(0x1F600, buf) == 4;
}

static int ae_tcl_is_high(const unsigned char* p) {   /* ED A0..AF xx: a high surrogate */
    return p[0] == 0xED && (p[1] & 0xF0) == 0xA0 && (p[2] & 0xC0) == 0x80;
}
static int ae_tcl_is_low(const unsigned char* p) {    /* ED B0..BF xx: a low surrogate */
    return p[0] == 0xED && (p[1] & 0xF0) == 0xB0 && (p[2] & 0xC0) == 0x80;
}

/* Tcl's string to UTF-8. *conv is NULL when `in` is already UTF-8 and may be
 * used as it is, else a malloc'd copy the caller frees. Anything but AE_STR_OK
 * is a refusal (and *conv is NULL). */
static int ae_tcl_to_utf8(const char* in, char** conv) {
    const unsigned char* p = (const unsigned char*)in;
    int pairs = 0;
    *conv = NULL;
    for (; *p; p++) {
        if (p[0] == 0xC0 && p[1] == 0x80) return AE_STR_NUL;
        if (p[0] == 0xED && (p[1] & 0xE0) == 0xA0) {         /* a surrogate, ED A0..BF */
            if (ae_tcl_is_high(p) && !ae_tcl_utf4_native() && p[3] && p[4] && p[5] && ae_tcl_is_low(p + 3)) { pairs++; p += 5; continue; }
            return AE_STR_SURROGATE;
        }
    }
    if (!pairs) return AE_STR_OK;
    char* out = (char*)malloc(strlen(in) + 1);
    if (!out) return AE_STR_SURROGATE;
    char* o = out;
    for (p = (const unsigned char*)in; *p; ) {
        if (ae_tcl_is_high(p) && p[3] && p[4] && p[5] && ae_tcl_is_low(p + 3)) {
            unsigned hi = 0xD000u | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
            unsigned lo = 0xD000u | ((p[4] & 0x3Fu) << 6) | (p[5] & 0x3Fu);
            unsigned cp = 0x10000u + (((hi & 0x3FFu) << 10) | (lo & 0x3FFu));
            *o++ = (char)(0xF0 | (cp >> 18));
            *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
            *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
            *o++ = (char)(0x80 | (cp & 0x3F));
            p += 6;
        } else {
            *o++ = (char)*p++;
        }
    }
    *o = 0;
    *conv = out;
    return AE_STR_OK;
}

static int ae_tcl_is_four(const unsigned char* p) {
    return p[0] >= 0xF0 && p[0] <= 0xF4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80;
}

/* UTF-8 to Tcl's string. A NUL cannot arrive in a C string; its overlong
 * spelling C0 80 would become one inside Tcl, so it is refused. On a Tcl with
 * 16-bit characters a four-byte sequence is split into the pair Tcl keeps. */
static int ae_tcl_from_utf8(const char* in, char** conv) {
    const unsigned char* p = (const unsigned char*)in;
    int fours = 0;
    *conv = NULL;
    for (; *p; p++) {
        if (p[0] == 0xC0 && p[1] == 0x80) return AE_STR_NUL;
        if (ae_tcl_is_four(p)) { fours++; p += 3; }
    }
    if (!fours || ae_tcl_utf4_native()) return AE_STR_OK;
    char* out = (char*)malloc(strlen(in) + fours * 2 + 1);
    if (!out) return AE_STR_SURROGATE;
    char* o = out;
    for (p = (const unsigned char*)in; *p; ) {
        if (ae_tcl_is_four(p)) {
            unsigned cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
            cp -= 0x10000u;
            unsigned hi = 0xD800u | (cp >> 10), lo = 0xDC00u | (cp & 0x3FFu);
            *o++ = (char)(0xE0 | (hi >> 12)); *o++ = (char)(0x80 | ((hi >> 6) & 0x3F)); *o++ = (char)(0x80 | (hi & 0x3F));
            *o++ = (char)(0xE0 | (lo >> 12)); *o++ = (char)(0x80 | ((lo >> 6) & 0x3F)); *o++ = (char)(0x80 | (lo & 0x3F));
            p += 4;
        } else {
            *o++ = (char)*p++;
        }
    }
    *o = 0;
    *conv = out;
    return AE_STR_OK;
}

/* A Tcl object holding the host's UTF-8 string, or NULL (with *status) when the
 * string cannot cross. */
static Tcl_Obj* ae_tcl_new_str(const char* in, int* status) {
    char* conv = NULL;
    int st = ae_tcl_from_utf8(in ? in : "", &conv);
    if (status) *status = st;
    if (st != AE_STR_OK) return NULL;
    Tcl_Obj* o = g_tcl.Tcl_NewStringObj(conv ? conv : (in ? in : ""), -1);
    free(conv);
    return o;
}

extern const char* aether_string_data(const void*);
extern size_t aether_string_length(const void*);

/* A string the host gave, as an Aether string with its length: a NUL inside it
 * (the length is more than the C string's) is refused here, since a C string
 * would have been cut at it. */
static Tcl_Obj* ae_tcl_new_host_str(const void* v, int* status) {
    const char* d = aether_string_data(v);
    if (!d) d = "";
    if (v && aether_string_length(v) != strlen(d)) {
        if (status) *status = AE_STR_NUL;
        return NULL;
    }
    return ae_tcl_new_str(d, status);
}

/* A refusal as a message: "<what>: refused: it holds <reason>". */
static void ae_tcl_refusal(char* buf, size_t n, const char* what, int status) {
    snprintf(buf, n, "%s: refused: it holds %s", what, ae_tcl_reason(status));
}

/* The program text for a script: converted, or NULL with a stderr line. */
static char* ae_tcl_code(const char* code) {
    char* conv = NULL;
    int st = ae_tcl_from_utf8(code, &conv);
    if (st != AE_STR_OK) {
        char msg[400];
        ae_tcl_refusal(msg, sizeof msg, "the script", st);
        fprintf(stderr, "[tcl] %s\n", msg);
        return NULL;
    }
    return conv ? conv : strdup(code);
}

/* The message a failed shared-interpreter run prints, as UTF-8. */
static void ae_tcl_print_error(Tcl_Interp* interp) {
    char* conv = NULL;
    const char* m = tcl_string_result(interp);
    int st = ae_tcl_to_utf8(m, &conv);
    if (st != AE_STR_OK) {
        char msg[400];
        ae_tcl_refusal(msg, sizeof msg, "the error message", st);
        fprintf(stderr, "[tcl] %s\n", msg);
    } else {
        fprintf(stderr, "[tcl] %s\n", conv ? conv : m);
    }
    free(conv);
}

static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

static int tcl_major = 0;

// Load libtcl on first use. In order:
//   1. ${AETHER_TCL_SONAME}: an exact soname or path (libtcl8.6.so.0,
//      /opt/homebrew/opt/tcl-tk/lib/libtcl9.0.dylib). Nothing else is tried
//      when it is set: the program that starts yours owns that choice.
//   2. The names a system with a Tcl runtime library installs, in this order:
//      Tcl 8.6, then 9.0, then 8.5 (8.6 first because it is what the tests
//      here have run against). Linux's libtclX.Y.so.0 (Debian,
//      Ubuntu, Fedora ship the runtime library under that name; the
//      unversioned libtcl.so comes with the dev package only), Homebrew's
//      tcl-tk, and macOS's own /usr/lib/libtcl.dylib (Tcl 8.5).
static int load_libtcl(void) {
    if (libtcl_handle) return 0;

    const char* env_soname = getenv("AETHER_TCL_SONAME");
    void* h = NULL;
    const char* tried = "";
    if (env_soname && *env_soname) {
        h = try_dlopen(env_soname);
        tried = env_soname;
    } else {
        static const char* const candidates[] = {
            "libtcl8.6.so.0", "libtcl8.6.so", "libtcl9.0.so.0", "libtcl9.0.so",
            "libtcl9.1.so.0", "libtcl9.1.so", "libtcl.so",
            "/opt/homebrew/opt/tcl-tk/lib/libtcl8.6.dylib",
            "/opt/homebrew/opt/tcl-tk/lib/libtcl9.0.dylib",
            "/opt/homebrew/opt/tcl-tk/lib/libtcl9.1.dylib",
            "/usr/local/opt/tcl-tk/lib/libtcl8.6.dylib",
            "/usr/local/opt/tcl-tk/lib/libtcl9.0.dylib",
            "/usr/local/opt/tcl-tk/lib/libtcl9.1.dylib",
            "/usr/lib/libtcl.dylib",
            NULL,
        };
        for (int i = 0; candidates[i] && !h; i++) h = try_dlopen(candidates[i]);
        tried = "libtcl8.6.so.0, libtcl9.0.so.0, libtcl9.1.so.0, libtcl.so, Homebrew's tcl-tk, /usr/lib/libtcl.dylib";
    }
    if (!h) {
        // dlerror() clears the error it reports: read it once.
        const char* why = dlerror();
        fprintf(stderr,
            "aether host_tcl: cannot dlopen libtcl "
            "(tried $AETHER_TCL_SONAME=%s, then %s).\n"
            "  Install a tcl runtime library on the host, or set "
            "AETHER_TCL_SONAME to the exact soname or path\n"
            "  (e.g. libtcl8.6.so.0; echo 'puts libtcl$tcl_version.so' | tclsh "
            "gives the unversioned name).\n  dlerror: %s\n",
            env_soname ? env_soname : "(unset)", tried, why ? why : "(none)");
        return -1;
    }

    if (resolve_tcl_symbols(h) != 0) {
        dlclose(h);
        return -1;
    }
    int major = 0, minor = 0, patch = 0, type = 0;
    g_tcl.Tcl_GetVersion(&major, &minor, &patch, &type);
    // 8.5, 8.6, 9.0 and 9.1 (the last seen on macOS in CI only) are what tcl_abi.h is written for. An 8.4 lacks the
    // limit functions (resolved above), and the layout argument was not made
    // for a Tcl past 9.
    if (major < 8 || major > 9 || (major == 8 && minor < 5)) {
        fprintf(stderr, "aether host_tcl: Tcl %d.%d is not supported (8.5, 8.6, 9.0 and 9.1 are)\n", major, minor);
        dlclose(h);
        return -1;
    }
    if (major == 9 && sizeof(long) != sizeof(long long)) {
        // Tcl 9's Tcl_Time.sec is a long long; tcl_abi.h declares a long.
        fprintf(stderr, "aether host_tcl: Tcl 9 on a platform where long is not 64 bits is not supported\n");
        dlclose(h);
        return -1;
    }
    tcl_major = major;
    libtcl_handle = h;
    return 0;
}

// --- bridge state (unchanged) ----------------------------------------------

static Tcl_Interp* T = NULL;

// Bridge-owned permission stack.

extern int list_size(void*);
extern void* list_get_raw(void*, int);
/* A grant list std.sandbox built holds plain C strings; one a program
 * built can hold Aether strings (a helper that keeps its `string`
 * parameter stores its own copy). This reads the bytes of either. */
extern const char* aether_string_data(const void*);

// The shared grant matcher (runtime/aether_sandbox_match.h).
#include "aether_sandbox_match.h"

// Tcl's one-time, process-wide initialisation, which an embedding application
// must do before it makes an interpreter. It is Tcl's state, not the bridge's
// (Tcl keeps its executable name, its encodings and, in 9.0, its zipfs mounts
// in the process), so it is made once per process and guarded: every
// interpreter this bridge makes, the shared one and each VM, calls it first,
// and a second call does nothing.
//
// Tcl 8.x: Tcl_FindExecutable(NULL), as before.
//
// Tcl 9.0 and later: a default build keeps its script library (init.tcl and
// the rest) in a zip archive attached to libtcl9.0.so itself, "zipfs", which
// the process has to mount before Tcl_Init can read it. tclsh does that in
// main() with TclZipfs_AppHook (unix/tclAppInit.c; Tcl 9's zipfs.3 documents
// it as the application's initialisation, and it calls Tcl_FindExecutable
// itself), which mounts a zip attached to the executable and the one attached
// to libtcl, and records where the library is mounted (//zipfs:/lib/tcl/
// tcl_library) for Tcl_Init's tclInit script to find through
// ::tcl::zipfs::tcl_library_init. TCL_LIBRARY, when set, is still read first
// by that script, and a build made with --disable-zipfs finds its library on
// disk as before. dlsym finds the hook only in a Tcl 9.
//
// The argv[0] it is given is a name that finds no executable. Tcl_FindExecutable
// documents that when it finds none, `info nameofexecutable` is empty, which is
// what 8.x gives here too (the bridge never had a name to give it), and it
// still records which shared library Tcl is running from, which is what the
// zipfs mount needs. NULL does not do: unix/tclUnixFile.c returns from
// TclpFindExecutable on a NULL argv0 before it records the library, so no
// zipfs is mounted and Tcl_Init finds no library. A real executable name is
// not given because Tcl would then open that file looking for a zip attached
// to it (the host program is not a Tcl application) and `info
// nameofexecutable` would name the host, which a script could exec.
static pthread_mutex_t process_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int process_ready = 0;

static void tcl_process_init(void) {
    pthread_mutex_lock(&process_init_lock);
    if (!process_ready) {
        if (g_tcl.TclZipfs_AppHook) {
            char name[] = "aether-embedded-tcl";   // no slash, found in no PATH directory
            char* argv0[2] = { name, NULL };
            char** argv = argv0;
            int argc = 1;
            g_tcl.TclZipfs_AppHook(&argc, &argv);
        } else {
            g_tcl.Tcl_FindExecutable(NULL);
        }
        process_ready = 1;
    }
    pthread_mutex_unlock(&process_init_lock);
}

// What to say after a failed Tcl_Init on a Tcl 9: its library may be in a zip
// attached to libtcl, which Tcl mounts by opening libtcl a second time, and a
// sandbox that does not grant a read of it makes that fail as "Cannot find a
// usable init.tcl".
static void tcl_init_hint(void) {
    if (g_tcl.TclZipfs_AppHook)
        fprintf(stderr,
            "[tcl] Tcl 9 keeps its script library in a zip attached to libtcl (zipfs) unless it was\n"
            "[tcl] built with --disable-zipfs: set TCL_LIBRARY to a directory that has it, or, under a\n"
            "[tcl] sandbox, grant fs_read on libtcl itself (tcl/README.md, \"Tcl 9 and its zipfs\")\n");
}

int tcl_init(void) {
    if (T) return 0;
    if (load_libtcl() != 0) return -1;
    tcl_process_init();
    T = g_tcl.Tcl_CreateInterp();
    if (!T) return -1;
    if (g_tcl.Tcl_Init(T) != TCL_OK) {
        fprintf(stderr, "[tcl] init: %s\n", tcl_string_result(T));
        tcl_init_hint();
        g_tcl.Tcl_DeleteInterp(T);
        T = NULL;
        return -1;
    }
    return 0;
}

void tcl_finalize(void) {
    if (T) {
        g_tcl.Tcl_DeleteInterp(T);
        T = NULL;
    }
    // Tcl_Finalize is final: a process that calls it cannot initialise Tcl
    // again (Tcl panics "Tcl_InitSubsystems called while exiting", in 8.6 and
    // 9.0 alike), so process_ready is left set.
    if (g_tcl.Tcl_Finalize) g_tcl.Tcl_Finalize();
}

int tcl_run(const char* code) {
    if (!code) return -1;
    if (tcl_init() != 0) return -1;
    char* c = ae_tcl_code(code);
    if (!c) return -1;
    int rc = tcl_eval(T, c);
    free(c);
    if (rc != TCL_OK) {
        ae_tcl_print_error(T);
        return -1;
    }
    return 0;
}

int tcl_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (tcl_init() != 0) return -1;
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;

    char* c = ae_tcl_code(code);
    if (!c) return -1;
    aether_sandbox_push(perms);

    int result = 0;
    if (tcl_eval(T, c) != TCL_OK) {
        ae_tcl_print_error(T);
        result = -1;
    }

    aether_sandbox_pop();
    free(c);

    return result;
}

// --- Shared map native bindings for Tcl ------------------------------------

static uint64_t current_map_token = 0;

/* A map command that was handed a string that cannot cross: a Tcl error the
 * script can catch, and nothing is stored or returned. */
static int ae_tcl_map_refuse(Tcl_Interp* interp, const char* what, int status) {
    char msg[400];
    ae_tcl_refusal(msg, sizeof msg, what, status);
    g_tcl.Tcl_SetObjResult(interp, g_tcl.Tcl_NewStringObj(msg, -1));
    return TCL_ERROR;
}

static int tcl_aether_map_get(ClientData cd, Tcl_Interp* interp,
                               Tcl_Size objc_wide, Tcl_Obj* const objv[]) {
    (void)cd;
    int objc = (int)objc_wide;   // an int in Tcl 8, a Tcl_Size in 9: the low 32 bits are the count
    if (objc != 2) {
        g_tcl.Tcl_WrongNumArgs(interp, 1, objv, "key");
        return TCL_ERROR;
    }
    char* kc = NULL;
    const char* key = tcl_obj_string(objv[1]);
    int st = ae_tcl_to_utf8(key, &kc);
    if (st != AE_STR_OK) return ae_tcl_map_refuse(interp, "aether_map_get: the key", st);
    const char* val = aether_shared_map_get_by_token(current_map_token, kc ? kc : key);
    free(kc);
    if (val) {
        Tcl_Obj* o = ae_tcl_new_str(val, &st);
        if (!o) return ae_tcl_map_refuse(interp, "aether_map_get: the value in the map", st);
        g_tcl.Tcl_SetObjResult(interp, o);
    } else {
        g_tcl.Tcl_SetObjResult(interp, g_tcl.Tcl_NewStringObj("", 0));
    }
    return TCL_OK;
}

static int tcl_aether_map_put(ClientData cd, Tcl_Interp* interp,
                               Tcl_Size objc_wide, Tcl_Obj* const objv[]) {
    (void)cd;
    int objc = (int)objc_wide;
    if (objc != 3) {
        g_tcl.Tcl_WrongNumArgs(interp, 1, objv, "key value");
        return TCL_ERROR;
    }
    char *kc = NULL, *vc = NULL;
    const char* key = tcl_obj_string(objv[1]);
    const char* val = tcl_obj_string(objv[2]);
    int st = ae_tcl_to_utf8(key, &kc);
    if (st != AE_STR_OK) return ae_tcl_map_refuse(interp, "aether_map_put: the key", st);
    st = ae_tcl_to_utf8(val, &vc);
    if (st != AE_STR_OK) { free(kc); return ae_tcl_map_refuse(interp, "aether_map_put: the value", st); }
    aether_shared_map_put_by_token(current_map_token, kc ? kc : key, vc ? vc : val);
    free(kc);
    free(vc);
    return TCL_OK;
}

static void register_map_bindings(Tcl_Interp* interp) {
    g_tcl.Tcl_CreateObjCommand(interp, "aether_map_get",
                                tcl_aether_map_get, NULL, NULL);
    g_tcl.Tcl_CreateObjCommand(interp, "aether_map_put",
                                tcl_aether_map_put, NULL, NULL);
}

int tcl_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    if (tcl_init() != 0) return -1;
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;
    register_map_bindings(T);

    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);

    current_map_token = map_token;

    char* c = ae_tcl_code(code);
    if (!c) { current_map_token = 0; return -1; }
    aether_sandbox_push(perms);

    int result = 0;
    if (tcl_eval(T, c) != TCL_OK) {
        ae_tcl_print_error(T);
        result = -1;
    }

    aether_sandbox_pop();
    free(c);
    current_map_token = 0;

    return result;
}


// --- Tier 2: Bidirectional Embedding API (#904) ----------------------------
//
// A VM is its own interpreter (Tcl_CreateInterp), separate from the shared
// one tcl_run uses: its own globals, procs and namespaces. Tcl's C API is
// command-oriented, not stack-oriented, so the Lua/Duktape shape maps over
// like this:
//   - a host callback is a Tcl command (Tcl_CreateObjCommand). Its name is
//     taken verbatim: "app.math.add" is one command a script calls as
//     `app.math.add 2 3` (Tcl allows dots in command names), and
//     "app::math::add" puts it in the namespace ::app::math;
//   - arg_* read the command's objv; push_* set the value the command
//     returns. A Tcl command returns one value, so the last push wins;
//   - an eval's result is the interpreter's result object, which the VM
//     holds a reference to until pop_result or the next eval.
//
// Tcl is apartment-threaded: an interpreter, and every Tcl_Obj it makes,
// belong to the thread that created them. An Aether program can reach a VM
// from any scheduler thread, so every entry point checks the caller is the
// creating thread and refuses otherwise, without touching Tcl.

#define AE_TCL_TNIL   0
#define AE_TCL_TBOOL  1
#define AE_TCL_TINT   2
#define AE_TCL_TSTR   3
#define AE_TCL_TTABLE 4
#define AE_TCL_TERR   5
#define AE_TCL_TOTHER 6
#define AE_TCL_TFLOAT 7

// What tcl_raise_error returns, so a host fn can `return tcl.raise_error(vm,
// msg)`. The trampoline turns it into TCL_ERROR after the host fn has
// returned, so the host fn's defers run.
#define AE_TCL_RAISE (-1)

#define AE_TCL_WRONG_THREAD "tcl vm used from a thread other than the one that created it"

/* One callback invocation: the command's arguments, and what the host fn
 * pushed or raised. A callback that evaluates on its own VM can reach a
 * second callback, so each trampoline keeps its own frame and restores the
 * one it interrupted. */
/* A UTF-8 copy of a Tcl string, kept until the call or result it belongs to
 * ends (the host reads a plain pointer). */
typedef struct AeTclBuf {
    struct AeTclBuf* next;
    char* s;
} AeTclBuf;

typedef struct AeTclFrame {
    int objc;
    Tcl_Obj* const* objv;
    Tcl_Obj* pushed;   /* one reference held, or NULL */
    Tcl_Obj* error;    /* one reference held, or NULL */
    AeTclBuf* bufs;    /* converted arguments, freed when the call ends */
    struct AeTclFrame* prev;
} AeTclFrame;

typedef struct {
    Tcl_Interp* interp;
    pthread_t owner;
    Tcl_Obj* result;          /* one reference held, or NULL */
    int result_is_err;
    const char* c_error;      /* a refusal made without touching Tcl */
    AeTclFrame* frame;
    int eval_depth;
    int64_t command_budget;   /* <= 0: no command limit */
    int command_granularity;
    int64_t time_budget_ms;   /* <= 0: no time limit */
    int time_granularity;
    AeTclBuf* result_bufs;    /* the converted result, freed at pop_result or the next eval */
    char pending[512];        /* a host-side string that could not cross, said at the next eval; "" for none */
} AeTclVm;

typedef struct {
    AeTclVm* vm;
    int (*host_fn)(void*);
} AeTclCommand;

static void ae_tcl_incr(Tcl_Obj* o) { g_tcl.Tcl_DbIncrRefCount(o, __FILE__, __LINE__); }
static void ae_tcl_decr(Tcl_Obj* o) { g_tcl.Tcl_DbDecrRefCount(o, __FILE__, __LINE__); }

/* Hold `o` in `*slot`, releasing what was there. The new reference is taken
 * first: `o` may be the object the slot already holds. */
static void ae_tcl_hold(Tcl_Obj** slot, Tcl_Obj* o) {
    if (o) ae_tcl_incr(o);
    if (*slot) ae_tcl_decr(*slot);
    *slot = o;
}

static void ae_tcl_bufs_free(AeTclBuf** list) {
    while (*list) {
        AeTclBuf* b = *list;
        *list = b->next;
        free(b->s);
        free(b);
    }
}

static int ae_tcl_on_owner(const AeTclVm* vm) {
    return vm && pthread_equal(vm->owner, pthread_self());
}

/* The value's type, from its internal representation. Every Tcl value is a
 * string, and one that has not been used as anything else has no other type:
 * the literal 5 in `set x 5` reads TSTR, `expr {2+3}` reads TINT. The
 * readers below convert from the string either way.
 *
 * Tcl_Obj's typePtr is read as a field: tcl_abi.h says why it is the same
 * word in Tcl 8.5, 8.6 and 9.0. Tcl_ObjType's first field is its name in all. */
static int ae_tcl_type_of(Tcl_Obj* o) {
    if (!o) return AE_TCL_TNIL;
    const Tcl_ObjType* t = o->typePtr;
    if (!t || !t->name) {
        /* A pure string. Empty is Tcl's "no value", what a command that
         * returns nothing leaves. Tcl's strings never hold a raw zero byte
         * (a NUL is stored as C0 80), so a leading one means empty. */
        const char* str = tcl_obj_string(o);
        return str && str[0] ? AE_TCL_TSTR : AE_TCL_TNIL;
    }
    const char* n = t->name;
    if (strcmp(n, "int") == 0 || strcmp(n, "wideInt") == 0) return AE_TCL_TINT;
    if (strcmp(n, "double") == 0) return AE_TCL_TFLOAT;
    if (strcmp(n, "boolean") == 0 || strcmp(n, "booleanString") == 0) return AE_TCL_TBOOL;
    if (strcmp(n, "list") == 0 || strcmp(n, "dict") == 0) return AE_TCL_TTABLE;
    if (strcmp(n, "bignum") == 0) return AE_TCL_TOTHER;
    /* Anything else (a command name, a bytearray, a parsed script) is
     * read as its string. */
    return AE_TCL_TSTR;
}

/* A value as int64. An integer that fits reads as itself; a double, or an
 * integer too wide for 64 bits, saturates (NaN reads 0), as a C cast of one
 * out of range would be undefined; anything that is not a number reads 0. */
static int64_t ae_tcl_to_i64(Tcl_Obj* o) {
    if (!o) return 0;
    Tcl_WideInt w;
    if (g_tcl.Tcl_GetWideIntFromObj(NULL, o, &w) == TCL_OK) return (int64_t)w;
    double d;
    if (g_tcl.Tcl_GetDoubleFromObj(NULL, o, &d) != TCL_OK) return 0;
    if (d != d) return 0;
    if (d >= 9223372036854775807.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)d;
}

static double ae_tcl_to_double(Tcl_Obj* o) {
    double d;
    if (!o || g_tcl.Tcl_GetDoubleFromObj(NULL, o, &d) != TCL_OK) return 0.0;
    return d;
}

/* Tcl's own rules: 1/0, true/false, yes/no, on/off. Anything else reads 0. */
static int ae_tcl_to_bool(Tcl_Obj* o) {
    int b;
    if (!o || g_tcl.Tcl_GetBooleanFromObj(NULL, o, &b) != TCL_OK) return 0;
    return b != 0;
}

/* The object's text as UTF-8, kept in `*keep` until that list is freed. A
 * string that cannot cross (status) reads "" and sets *status. */
static const char* ae_tcl_to_str(Tcl_Obj* o, AeTclBuf** keep, int* status) {
    if (status) *status = AE_STR_OK;
    if (!o) return "";
    const char* s = tcl_obj_string(o);
    if (!s) return "";
    char* conv = NULL;
    int st = ae_tcl_to_utf8(s, &conv);
    if (st != AE_STR_OK) {
        if (status) *status = st;
        return "";
    }
    if (!conv) return s;
    AeTclBuf* b = (AeTclBuf*)malloc(sizeof *b);
    if (!b) { free(conv); return ""; }
    b->s = conv;
    b->next = *keep;
    *keep = b;
    return conv;
}

void* tcl_vm_new(void) {
    if (load_libtcl() != 0) return NULL;
    tcl_process_init();
    Tcl_Interp* interp = g_tcl.Tcl_CreateInterp();
    if (!interp) return NULL;
    if (g_tcl.Tcl_Init(interp) != TCL_OK) {
        fprintf(stderr, "[tcl] vm init: %s\n", tcl_string_result(interp));
        tcl_init_hint();
        g_tcl.Tcl_DeleteInterp(interp);
        return NULL;
    }
    AeTclVm* vm = (AeTclVm*)calloc(1, sizeof(AeTclVm));
    if (!vm) {
        g_tcl.Tcl_DeleteInterp(interp);
        return NULL;
    }
    vm->interp = interp;
    vm->owner = pthread_self();
    return vm;
}

void tcl_vm_free(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm) return;
    if (!ae_tcl_on_owner(vm)) {
        /* Its interpreter can only be deleted on its own thread: leaked,
         * and said so, rather than deleted from the wrong one. */
        fprintf(stderr, "[tcl] vm_free: %s; not freed\n", AE_TCL_WRONG_THREAD);
        return;
    }
    ae_tcl_hold(&vm->result, NULL);
    ae_tcl_bufs_free(&vm->result_bufs);
    /* Deleting the interpreter deletes its commands, whose delete procs free
     * their AeTclCommand records. */
    g_tcl.Tcl_DeleteInterp(vm->interp);
    free(vm);
}

static void ae_tcl_command_deleted(ClientData cd) { free(cd); }

static int ae_tcl_trampoline(ClientData cd, Tcl_Interp* interp,
                             Tcl_Size objc_wide, Tcl_Obj* const objv[]) {
    int objc = (int)objc_wide;   // see tcl_abi.h: int in Tcl 8, Tcl_Size in 9
    AeTclCommand* cmd = (AeTclCommand*)cd;
    AeTclVm* vm = cmd->vm;
    AeTclFrame frame = {objc, objv, NULL, NULL, NULL, vm->frame};
    /* A string argument the host could not be given whole is refused before
     * the host function runs: the script gets a Tcl error that names the
     * command and the argument, and the host function is not called. */
    for (int i = 1; i < objc; i++) {
        int t = ae_tcl_type_of(objv[i]);
        if (t != AE_TCL_TSTR && t != AE_TCL_TTABLE && t != AE_TCL_TOTHER) continue;
        char* conv = NULL;
        int st = ae_tcl_to_utf8(tcl_obj_string(objv[i]), &conv);
        free(conv);
        if (st != AE_STR_OK) {
            char what[200], msg[800];
            snprintf(what, sizeof what, "%s: argument %d", tcl_obj_string(objv[0]), i);
            ae_tcl_refusal(msg, sizeof msg, what, st);
            g_tcl.Tcl_SetObjResult(interp, g_tcl.Tcl_NewStringObj(msg, -1));
            return TCL_ERROR;
        }
    }
    vm->frame = &frame;
    int rc = cmd->host_fn((void*)vm);
    vm->frame = frame.prev;
    ae_tcl_bufs_free(&frame.bufs);

    int status = TCL_OK;
    if (frame.error) {
        g_tcl.Tcl_SetObjResult(interp, frame.error);
        status = TCL_ERROR;
    } else if (rc > 0 && frame.pushed) {
        g_tcl.Tcl_SetObjResult(interp, frame.pushed);
    } else {
        g_tcl.Tcl_ResetResult(interp);
    }
    ae_tcl_hold(&frame.pushed, NULL);
    ae_tcl_hold(&frame.error, NULL);
    return status;
}

int tcl_vm_register(void* p, const char* name, void* host_fn) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !name || !*name || !host_fn || !ae_tcl_on_owner(vm)) return -1;
    AeTclCommand* cmd = (AeTclCommand*)malloc(sizeof(AeTclCommand));
    if (!cmd) return -1;
    cmd->vm = vm;
    cmd->host_fn = (int (*)(void*))host_fn;
    if (!g_tcl.Tcl_CreateObjCommand(vm->interp, name, ae_tcl_trampoline, cmd,
                                    ae_tcl_command_deleted)) {
        free(cmd);
        return -1;
    }
    return 0;
}

static void ae_tcl_set_global(AeTclVm* vm, const char* name, Tcl_Obj* value) {
    ae_tcl_incr(value);
    g_tcl.Tcl_SetVar2Ex(vm->interp, name, NULL, value, TCL_GLOBAL_ONLY);
    ae_tcl_decr(value);
}

void tcl_vm_set_global_str(void* p, const char* name, const void* value) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !name || !ae_tcl_on_owner(vm)) return;
    int st;
    Tcl_Obj* o = ae_tcl_new_host_str(value, &st);
    if (!o) {
        char what[200];
        snprintf(what, sizeof what, "tcl_vm_set_global_str(%s): the value", name);
        ae_tcl_refusal(vm->pending, sizeof vm->pending, what, st);
        return;
    }
    ae_tcl_set_global(vm, name, o);
}

void tcl_vm_set_global_int(void* p, const char* name, int64_t value) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !name || !ae_tcl_on_owner(vm)) return;
    ae_tcl_set_global(vm, name, g_tcl.Tcl_NewWideIntObj((Tcl_WideInt)value));
}

/* A Tcl list of the strings in an Aether list. */
void tcl_vm_set_global_strlist(void* p, const char* name, void* items) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !name || !ae_tcl_on_owner(vm)) return;
    Tcl_Obj* list = g_tcl.Tcl_NewListObj(0, NULL);
    if (items) {
        int count = list_size(items);
        for (int i = 0; i < count; i++) {
            void* item = list_get_raw(items, i);
            const char* str = item ? aether_string_data(item) : "";
            int st;
            Tcl_Obj* o = ae_tcl_new_str(str, &st);
            if (!o) {
                char what[200];
                snprintf(what, sizeof what, "tcl_vm_set_global_strlist(%s): item %d", name, i);
                ae_tcl_refusal(vm->pending, sizeof vm->pending, what, st);
                ae_tcl_incr(list);
                ae_tcl_decr(list);
                return;
            }
            g_tcl.Tcl_ListObjAppendElement(NULL, list, o);
        }
    }
    ae_tcl_set_global(vm, name, list);
}

/* Arms the limits for one outermost eval. A callback's nested eval runs
 * inside the same budget rather than starting a fresh one, or a script
 * could renew its own allowance through any callback that evaluates. */
static void ae_tcl_arm_limits(AeTclVm* vm) {
    Tcl_Interp* in = vm->interp;
    if (vm->command_budget > 0) {
        /* The command limit is a ceiling on the interpreter's running
         * command count, so the budget is added to the count now. */
        g_tcl.Tcl_LimitTypeReset(in, TCL_LIMIT_COMMANDS);
        int64_t now = 0;
        if (tcl_eval(in, "info cmdcount") == TCL_OK) now = ae_tcl_to_i64(g_tcl.Tcl_GetObjResult(in));
        int64_t ceiling = now + vm->command_budget;
        if (ceiling > INT_MAX) ceiling = INT_MAX;
        g_tcl.Tcl_LimitSetCommands(in, (Tcl_Size)ceiling);
        g_tcl.Tcl_LimitSetGranularity(in, TCL_LIMIT_COMMANDS, vm->command_granularity);
        g_tcl.Tcl_LimitTypeSet(in, TCL_LIMIT_COMMANDS);
    }
    if (vm->time_budget_ms > 0) {
        g_tcl.Tcl_LimitTypeReset(in, TCL_LIMIT_TIME);
        Tcl_Time t;
        g_tcl.Tcl_GetTime(&t);
        int64_t usec = (int64_t)t.usec + (vm->time_budget_ms % 1000) * 1000;
        t.sec += vm->time_budget_ms / 1000 + usec / 1000000;
        t.usec = usec % 1000000;
        g_tcl.Tcl_LimitSetTime(in, &t);
        g_tcl.Tcl_LimitSetGranularity(in, TCL_LIMIT_TIME, vm->time_granularity);
        g_tcl.Tcl_LimitTypeSet(in, TCL_LIMIT_TIME);
    }
}

/* Clears both limits, and the interpreter's "exceeded" state with them, so
 * the VM evaluates again after a run that hit one. */
static void ae_tcl_disarm_limits(AeTclVm* vm) {
    g_tcl.Tcl_LimitTypeReset(vm->interp, TCL_LIMIT_COMMANDS);
    g_tcl.Tcl_LimitTypeReset(vm->interp, TCL_LIMIT_TIME);
}

/* What a script that ended in `return` completes with, as Tcl decides it for
 * an outermost eval: `return -code c` one level up is c (`return v` is ok,
 * `return -code error m` an error); a return aimed further up stays
 * TCL_RETURN, which the caller reports as a bad code, as Tcl does. */
static int ae_tcl_return_code(Tcl_Interp* interp) {
    Tcl_Obj* opts = g_tcl.Tcl_GetReturnOptions(interp, TCL_RETURN);
    ae_tcl_incr(opts);
    int code = TCL_OK, level = 1;
    Tcl_Obj* key = g_tcl.Tcl_NewStringObj("-code", -1);
    Tcl_Obj* v = NULL;
    ae_tcl_incr(key);
    if (g_tcl.Tcl_DictObjGet(NULL, opts, key, &v) == TCL_OK && v) g_tcl.Tcl_GetIntFromObj(NULL, v, &code);
    ae_tcl_decr(key);
    key = g_tcl.Tcl_NewStringObj("-level", -1);
    ae_tcl_incr(key);
    v = NULL;
    if (g_tcl.Tcl_DictObjGet(NULL, opts, key, &v) == TCL_OK && v) g_tcl.Tcl_GetIntFromObj(NULL, v, &level);
    ae_tcl_decr(key);
    ae_tcl_decr(opts);
    return level <= 1 ? code : TCL_RETURN;
}

/* Runs `code` at global level, even from a callback a proc made, so a
 * callback's nested eval cannot see or set the proc's locals. 0 on success;
 * -1 on an error, which is then the result (TCL_TERR, its message as the
 * string). `return v` succeeds with v, and a `break` or `continue` outside
 * a loop is an error. Tcl_EvalObjEx does both itself only for the outermost
 * eval; for one nested in a callback, the code below does.
 *
 * The script goes in as an object (Tcl_EvalObjEx), which compiles it whole,
 * not as a string (Tcl_EvalEx), which runs it a command at a time. Tcl checks
 * its limits only inside compiled code, every few dozen instructions, so a
 * top-level `while 1 {incr i}` run as a string never reaches a check: each
 * pass of its body is a fresh compiled run too short to get there. */
int tcl_vm_eval(void* p, const void* codev) {
    const char* code = aether_string_data(codev);
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm) return -1;
    if (!ae_tcl_on_owner(vm)) {
        vm->c_error = AE_TCL_WRONG_THREAD;
        return -1;
    }
    if (!code) return -1;
    vm->c_error = NULL;
    ae_tcl_bufs_free(&vm->result_bufs);
    /* A host-side string that could not cross (a global it set) or a script
     * that cannot: the eval does not run, and says why. */
    int cst = AE_STR_OK;
    Tcl_Obj* script = NULL;
    if (vm->pending[0]) {
        script = NULL;
        cst = -1;
    } else {
        script = ae_tcl_new_host_str(codev, &cst);
    }
    if (!script) {
        char msg[600];
        if (cst == -1) { snprintf(msg, sizeof msg, "%s", vm->pending); vm->pending[0] = 0; }
        else ae_tcl_refusal(msg, sizeof msg, "the script", cst);
        ae_tcl_hold(&vm->result, g_tcl.Tcl_NewStringObj(msg, -1));
        vm->result_is_err = 1;
        return -1;
    }
    if (vm->eval_depth == 0) ae_tcl_arm_limits(vm);
    vm->eval_depth++;
    ae_tcl_incr(script);
    int rc = g_tcl.Tcl_EvalObjEx(vm->interp, script, TCL_EVAL_GLOBAL);
    ae_tcl_decr(script);
    vm->eval_depth--;
    if (rc == TCL_RETURN) rc = ae_tcl_return_code(vm->interp);
    if (rc != TCL_OK && rc != TCL_ERROR) {
        char msg[64];
        if (rc == TCL_BREAK) snprintf(msg, sizeof msg, "invoked \"break\" outside of a loop");
        else if (rc == TCL_CONTINUE) snprintf(msg, sizeof msg, "invoked \"continue\" outside of a loop");
        else snprintf(msg, sizeof msg, "command returned bad code: %d", rc);
        g_tcl.Tcl_SetObjResult(vm->interp, g_tcl.Tcl_NewStringObj(msg, -1));
        rc = TCL_ERROR;
    }
    ae_tcl_hold(&vm->result, g_tcl.Tcl_GetObjResult(vm->interp));
    vm->result_is_err = rc == TCL_ERROR;
    /* A result or an error message with a NUL (or a lone surrogate) in it
     * cannot be handed to the host as the string it is: it becomes an error
     * that says so. */
    {
        int t = ae_tcl_type_of(vm->result);
        if (t == AE_TCL_TSTR || t == AE_TCL_TTABLE || t == AE_TCL_TOTHER) {
            char* conv = NULL;
            int st = ae_tcl_to_utf8(tcl_obj_string(vm->result), &conv);
            free(conv);
            if (st != AE_STR_OK) {
                char msg[600];
                ae_tcl_refusal(msg, sizeof msg, rc == TCL_ERROR ? "the script's error message" : "the script's result", st);
                ae_tcl_hold(&vm->result, g_tcl.Tcl_NewStringObj(msg, -1));
                vm->result_is_err = 1;
                rc = TCL_ERROR;
            }
        }
    }
    if (vm->eval_depth == 0) ae_tcl_disarm_limits(vm);
    g_tcl.Tcl_ResetResult(vm->interp);
    return rc == TCL_ERROR ? -1 : 0;
}

int tcl_vm_result_type(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm) return AE_TCL_TNIL;
    if (vm->c_error) return AE_TCL_TERR;
    if (!ae_tcl_on_owner(vm) || !vm->result) return AE_TCL_TNIL;
    if (vm->result_is_err) return AE_TCL_TERR;
    return ae_tcl_type_of(vm->result);
}

int64_t tcl_vm_result_int(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || vm->c_error || !ae_tcl_on_owner(vm)) return 0;
    return ae_tcl_to_i64(vm->result);
}

double tcl_vm_result_double(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || vm->c_error || !ae_tcl_on_owner(vm)) return 0.0;
    return ae_tcl_to_double(vm->result);
}

/* Valid until pop_result or the VM's next eval. */
const char* tcl_vm_result_str(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm) return "";
    if (vm->c_error) return vm->c_error;
    if (!ae_tcl_on_owner(vm)) return "";
    return ae_tcl_to_str(vm->result, &vm->result_bufs, NULL);
}

int tcl_vm_result_bool(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || vm->c_error || !ae_tcl_on_owner(vm)) return 0;
    return ae_tcl_to_bool(vm->result);
}

void tcl_vm_pop_result(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm) return;
    vm->c_error = NULL;
    if (!ae_tcl_on_owner(vm)) return;
    ae_tcl_hold(&vm->result, NULL);
    ae_tcl_bufs_free(&vm->result_bufs);
    vm->result_is_err = 0;
}

/* A budget of Tcl commands per outermost eval, checked every `every`
 * commands; `budget` <= 0 removes it. Commands are what Tcl counts, so a
 * loop that runs none (`while 1 {}`, compiled to jumps) is not bounded by
 * it: tcl_vm_set_time_limit is. "" when set. */
const char* tcl_vm_set_instruction_limit(void* p, int64_t budget, int every) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm) return "tcl vm is null";
    if (!ae_tcl_on_owner(vm)) return AE_TCL_WRONG_THREAD;
    vm->command_budget = budget;
    vm->command_granularity = every > 0 ? every : 1;
    return "";
}

/* A wall-clock budget in milliseconds per outermost eval, checked every
 * `every` checkpoints; `ms` <= 0 removes it. Tcl checks it on a loop's
 * backward jump too, so it bounds the loops the command limit does not.
 * Time spent inside a host callback counts. "" when set. */
const char* tcl_vm_set_time_limit(void* p, int64_t ms, int every) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm) return "tcl vm is null";
    if (!ae_tcl_on_owner(vm)) return AE_TCL_WRONG_THREAD;
    vm->time_budget_ms = ms;
    vm->time_granularity = every > 0 ? every : 1;
    return "";
}

/* The current callback's argument `i` (1-based; the command's name is not
 * one), or NULL outside a callback or past the last. */
static Tcl_Obj* ae_tcl_arg(void* p, int i) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !vm->frame || i < 1 || i >= vm->frame->objc) return NULL;
    return vm->frame->objv[i];
}

int tcl_arg_count(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !vm->frame) return 0;
    return vm->frame->objc - 1;
}

int tcl_arg_type(void* p, int i) { return ae_tcl_type_of(ae_tcl_arg(p, i)); }
int64_t tcl_arg_int(void* p, int i) { return ae_tcl_to_i64(ae_tcl_arg(p, i)); }
double tcl_arg_double(void* p, int i) { return ae_tcl_to_double(ae_tcl_arg(p, i)); }
int tcl_arg_bool(void* p, int i) { return ae_tcl_to_bool(ae_tcl_arg(p, i)); }
/* Valid until the callback returns. */
const char* tcl_arg_str(void* p, int i) {
    AeTclVm* vm = (AeTclVm*)p;
    /* The trampoline refused any string argument that cannot cross before the
     * host function ran, so there is a frame and this converts only. */
    return ae_tcl_to_str(ae_tcl_arg(p, i), vm && vm->frame ? &vm->frame->bufs : &vm->result_bufs, NULL);
}

static void ae_tcl_push(void* p, Tcl_Obj* o) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !vm->frame) return;
    ae_tcl_hold(&vm->frame->pushed, o);
}

static int ae_tcl_in_callback(void* p) {
    return p && ((AeTclVm*)p)->frame != NULL;
}

void tcl_push_int(void* p, int64_t v) {
    if (ae_tcl_in_callback(p)) ae_tcl_push(p, g_tcl.Tcl_NewWideIntObj((Tcl_WideInt)v));
}
void tcl_push_double(void* p, double v) {
    if (ae_tcl_in_callback(p)) ae_tcl_push(p, g_tcl.Tcl_NewDoubleObj(v));
}
/* Tcl has no boolean type: true is 1, false is 0. */
void tcl_push_bool(void* p, int v) {
    if (ae_tcl_in_callback(p)) ae_tcl_push(p, g_tcl.Tcl_NewWideIntObj(v != 0));
}
void tcl_push_str(void* p, const void* v) {
    if (!ae_tcl_in_callback(p)) return;
    int st;
    Tcl_Obj* o = ae_tcl_new_host_str(v, &st);
    if (o) { ae_tcl_push(p, o); return; }
    /* The host's string cannot be given to the script as it is: the call fails
     * with the reason, in the script, instead of returning something else. */
    char msg[600];
    ae_tcl_refusal(msg, sizeof msg, "the value the host returned", st);
    ae_tcl_hold(&((AeTclVm*)p)->frame->error, g_tcl.Tcl_NewStringObj(msg, -1));
}
/* Tcl has no nil: the empty string, which reads back TCL_TNIL. */
void tcl_push_nil(void* p) {
    if (ae_tcl_in_callback(p)) ae_tcl_push(p, NULL);
}

int tcl_raise_error(void* p, const void* msg) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !vm->frame) return 0;
    int st;
    Tcl_Obj* o = msg ? ae_tcl_new_host_str(msg, &st) : ae_tcl_new_str("error", &st);
    if (!o) {
        char m2[600];
        ae_tcl_refusal(m2, sizeof m2, "the error message the host raised", st);
        o = g_tcl.Tcl_NewStringObj(m2, -1);
    }
    ae_tcl_hold(&vm->frame->error, o);
    return AE_TCL_RAISE;
}

#else
#include <stdio.h>
int tcl_init(void) {
    fprintf(stderr, "error: tcl not available (compiled with AETHER_HAS_TCL=0)\n");
    return -1;
}
void tcl_finalize(void) {}
int tcl_run(const char* code) { (void)code; return tcl_init(); }
int tcl_run_sandboxed(void* perms, const char* code) {
    (void)perms; (void)code; return tcl_init();
}
int tcl_run_sandboxed_with_map(void* perms, const char* code, uint64_t token) {
    (void)perms; (void)code; (void)token; return tcl_init();
}
void* tcl_vm_new(void) { tcl_init(); return NULL; }
void tcl_vm_free(void* vm) { (void)vm; }
int tcl_vm_register(void* vm, const char* name, void* host_fn) { (void)vm; (void)name; (void)host_fn; return -1; }
void tcl_vm_set_global_str(void* vm, const char* name, const void* value) { (void)vm; (void)name; (void)value; }
void tcl_vm_set_global_int(void* vm, const char* name, int64_t value) { (void)vm; (void)name; (void)value; }
void tcl_vm_set_global_strlist(void* vm, const char* name, void* items) { (void)vm; (void)name; (void)items; }
int tcl_vm_eval(void* vm, const void* code) { (void)vm; (void)code; return -1; }
int tcl_vm_result_type(void* vm) { (void)vm; return 0; }
int64_t tcl_vm_result_int(void* vm) { (void)vm; return 0; }
double tcl_vm_result_double(void* vm) { (void)vm; return 0.0; }
const char* tcl_vm_result_str(void* vm) { (void)vm; return ""; }
int tcl_vm_result_bool(void* vm) { (void)vm; return 0; }
void tcl_vm_pop_result(void* vm) { (void)vm; }
const char* tcl_vm_set_instruction_limit(void* vm, int64_t budget, int every) { (void)vm; (void)budget; (void)every; return "contrib.host.tcl not available"; }
const char* tcl_vm_set_time_limit(void* vm, int64_t ms, int every) { (void)vm; (void)ms; (void)every; return "contrib.host.tcl not available"; }
int tcl_arg_count(void* vm) { (void)vm; return 0; }
int tcl_arg_type(void* vm, int i) { (void)vm; (void)i; return 0; }
int64_t tcl_arg_int(void* vm, int i) { (void)vm; (void)i; return 0; }
double tcl_arg_double(void* vm, int i) { (void)vm; (void)i; return 0.0; }
int tcl_arg_bool(void* vm, int i) { (void)vm; (void)i; return 0; }
const char* tcl_arg_str(void* vm, int i) { (void)vm; (void)i; return ""; }
void tcl_push_int(void* vm, int64_t v) { (void)vm; (void)v; }
void tcl_push_double(void* vm, double v) { (void)vm; (void)v; }
void tcl_push_bool(void* vm, int v) { (void)vm; (void)v; }
void tcl_push_str(void* vm, const void* v) { (void)vm; (void)v; }
void tcl_push_nil(void* vm) { (void)vm; }
int tcl_raise_error(void* vm, const void* msg) { (void)vm; (void)msg; return 0; }
#endif
