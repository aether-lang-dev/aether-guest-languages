// duktape_abi.h: the part of Duktape's C API that aether_host_duktape.c
// declares itself.
//
// The bridge loads libduktape with dlopen and finds every function with
// dlsym, so it needs Duktape's types and a few constants at compile time,
// never a symbol at link time. In aether's tree it took them from the
// system's duktape.h (libduktape-dev), found by `make contrib`'s pkg-config
// probe. Here the bridge is compiled into the program that imports it
// (`@source` in module.ae), where no such probe runs, so it carries the
// declarations it uses instead. A machine that runs a program built with it
// needs the Duktape runtime library only, not the dev package.
//
// These are Duktape 2.x's own definitions under its default duk_config.h,
// which every distribution and Homebrew ship for 2.x (libduktape.so.207 is
// 2.7): duk_int_t is `int`, duk_uint_t `unsigned int`, duk_idx_t and
// duk_errcode_t duk_int_t, duk_ret_t `int`, duk_bool_t `unsigned int`,
// duk_double_t `double`, duk_size_t `size_t`. A libduktape built with a
// non-default duk_config.h that changes those integer typedefs is not
// supported (nor was it with the system headers: they describe the build
// host's libduktape, not the deploy host's, which is the one dlopen finds).
#ifndef AETHER_GUEST_DUKTAPE_ABI_H
#define AETHER_GUEST_DUKTAPE_ABI_H

#include <stddef.h>
#include <stdint.h>

typedef struct duk_hthread duk_context;     // only ever a pointer here

typedef int duk_int_t;
typedef unsigned int duk_uint_t;
typedef int duk_small_int_t;
typedef unsigned int duk_small_uint_t;
typedef duk_int_t duk_idx_t;
typedef duk_uint_t duk_uarridx_t;
typedef duk_small_int_t duk_ret_t;
typedef duk_small_uint_t duk_bool_t;
typedef duk_int_t duk_errcode_t;
typedef double duk_double_t;
typedef size_t duk_size_t;

typedef duk_ret_t (*duk_c_function)(duk_context *ctx);
typedef void *(*duk_alloc_function)(void *udata, duk_size_t size);
typedef void *(*duk_realloc_function)(void *udata, void *ptr, duk_size_t size);
typedef void (*duk_free_function)(void *udata, void *ptr);
typedef void (*duk_fatal_function)(void *udata, const char *msg);

// A C function declared with this many arguments takes however many it is
// called with.
#define DUK_VARARGS ((duk_int_t) (-1))

// duk_get_type()
#define DUK_TYPE_NONE       0U
#define DUK_TYPE_UNDEFINED  1U
#define DUK_TYPE_NULL       2U
#define DUK_TYPE_BOOLEAN    3U
#define DUK_TYPE_NUMBER     4U
#define DUK_TYPE_STRING     5U
#define DUK_TYPE_OBJECT     6U
#define DUK_TYPE_BUFFER     7U
#define DUK_TYPE_POINTER    8U
#define DUK_TYPE_LIGHTFUNC  9U

// duk_eval_raw() flags, which duk_peval_string() is a macro over.
#define DUK_COMPILE_EVAL        (1U << 3)
#define DUK_COMPILE_SAFE        (1U << 7)
#define DUK_COMPILE_NOSOURCE    (1U << 9)
#define DUK_COMPILE_STRLEN      (1U << 10)
#define DUK_COMPILE_NOFILENAME  (1U << 11)

#endif
