// racket_abi.h: the part of Racket CS's embedding API (racketcs.h,
// racketcsboot.h) and of Chez Scheme's C API (chezscheme.h) that
// aether_host_racket.c declares itself.
//
// The bridge loads Racket CS's embedding library with dlopen and finds every
// function with dlsym, so it needs the types, a few constants and the Chez
// object macros at compile time, never a symbol at link time. In aether's
// tree it took them from chezscheme.h and racketcs.h of a built Racket and
// linked libracketcs.a statically. Here the bridge is compiled into the
// program that imports it (`@source` in module.ae), where no Racket headers
// are, so it carries what it uses. A machine that runs a program built with
// it needs Racket's runtime only (the library, the boot files, the
// collects), not a Racket build tree.
//
// What is copied, and what it ties the bridge to:
//
//  * Chez's tagged-pointer constants and macros (Snil, Spairp, Scar,
//    Sbytevector_length, ...): these are Chez's 64-bit object layout, the
//    same in Chez 9.5 (Racket 8.x) and Chez 10 (Racket 9.x) on x86_64 and
//    arm64, little endian. A 32-bit Racket is not supported.
//  * racket_boot_arguments_t, which changed in Racket 8.14: `bootN_data`
//    fields were added between the path and the offset. Both layouts are
//    here, and the bridge picks by the library it loaded (racket_abi_new_boot).
//
// Everything else the bridge calls is a real exported function, found with
// dlsym: racket_boot, racket_eval, racket_apply, racket_primitive,
// racket_namespace_require, racket_dynamic_require, and Chez's
// Sstring_to_symbol, Scons, Smake_bytevector, Sstring_utf8, Sinteger,
// Sinteger_value.
#ifndef AETHER_GUEST_RACKET_ABI_H
#define AETHER_GUEST_RACKET_ABI_H

#include <stddef.h>
#include <stdint.h>

typedef void* ptr;                  // a Chez Scheme object
typedef intptr_t iptr;
typedef uintptr_t uptr;

// Chez's immediate objects (64-bit).
#define Sfalse ((ptr)0x6)
#define Strue ((ptr)0xE)
#define Snil ((ptr)0x26)
#define Svoid ((ptr)0x2E)

#define Sfixnum(x) ((ptr)(uptr)((x) * 8))
#define Sfixnump(x) (((uptr)(x) & 0x7) == 0x0)
#define Sfixnum_value(x) ((iptr)(x) / 8)
#define Spairp(x) (((uptr)(x) & 0x7) == 0x1)
#define Scar(x) (*((ptr*)(void*)((uptr)(x) + 7)))
#define Scdr(x) (*((ptr*)(void*)((uptr)(x) + 15)))
#define Sbytevectorp(x) ((((uptr)(x) & 0x7) == 0x7) && \
    (((uptr)(*((ptr*)(void*)((uptr)(x) + 1))) & 0x3) == 0x1))
#define Sbytevector_length(x) ((iptr)((uptr)(*((iptr*)(void*)((uptr)(x) + 1))) >> 3))
#define Sbytevector_data(x) (&((unsigned char*)(void*)((uptr)(x) + 9))[0])

// racketcsboot.h, Racket 8.10 to 8.13: boot images by path only.
typedef struct racket_boot_arguments_old_t {
    const char* boot1_path;
    long boot1_offset;
    long boot1_len;
    const char* boot2_path;
    long boot2_offset;
    long boot2_len;
    const char* boot3_path;
    long boot3_offset;
    long boot3_len;
    int argc;
    char** argv;
    const char* exec_file;
    const char* run_file;
    const char* collects_dir;
    const char* config_dir;
    void* dll_dir;
    const char* k_file;
    int cs_compiled_subdir;
    long segment_offset;
    void* dll_open;
    void* dll_find_object;
    void* dll_close;
    int exit_after;
    int is_gui;
    int wm_is_gracket_or_x11_arg_count;
    char* gracket_guid_or_x11_args;
} racket_boot_arguments_old_t;

// racketcsboot.h, Racket 8.14 and later (9.x checked): a `bootN_data` after
// each path.
typedef struct racket_boot_arguments_new_t {
    const char* boot1_path;
    void* boot1_data;
    long boot1_offset;
    long boot1_len;
    const char* boot2_path;
    void* boot2_data;
    long boot2_offset;
    long boot2_len;
    const char* boot3_path;
    void* boot3_data;
    long boot3_offset;
    long boot3_len;
    int argc;
    char** argv;
    const char* exec_file;
    const char* run_file;
    const char* collects_dir;
    const char* config_dir;
    void* dll_dir;
    const char* k_file;
    int cs_compiled_subdir;
    long segment_offset;
    void* dll_open;
    void* dll_find_object;
    void* dll_close;
    int exit_after;
    int is_gui;
    int wm_is_gracket_or_x11_arg_count;
    char* gracket_guid_or_x11_args;
} racket_boot_arguments_new_t;

#endif
