// factor_abi.h: the types the bridge uses to call libfactor, so that
// building it needs no Factor source or headers: libfactor is loaded with
// dlopen and every entry point through dlsym (aether_host_factor.c).
//
// libfactor here is the embed-api fork's (README.md, "Caveat 1"): upstream
// Factor has no re-entrant entry point, so none of these exist in a stock
// libfactor. The three entry points are the fork's `vm/embed_api.cpp`, plain
// C (`VM_C_API`), with only char pointers, void and function pointers in
// their signatures, so the same declarations hold on every platform the fork
// builds for. Nothing else of Factor's is used: no header, no struct, no
// constant.
#ifndef AETHER_FACTOR_ABI_H
#define AETHER_FACTOR_ABI_H

// char* factor_embed_eval(const char* image_path, const char* src)
//   Starts the VM on first call from the bootstrapped factor.image at
//   image_path (NULL: the VM's default), then evaluates src in it and
//   returns what it printed (eval>string) as a malloc'd string. NULL on
//   failure. The VM is persistent: later calls reuse it.
typedef char* (*factor_embed_eval_fn)(const char* image_path, const char* src);

// void factor_embed_eval_free(char* result)
//   Frees what factor_embed_eval returned (it is allocated by libfactor).
typedef void (*factor_embed_eval_free_fn)(char* result);

// The host key-value hooks. get returns a C string the host owns, valid
// until the next call, or NULL; Factor copies it at once. put's strings are
// borrowed for the call.
typedef const char* (*factor_map_get_fn)(const char* key);
typedef void        (*factor_map_put_fn)(const char* key, const char* value);

// void factor_embed_map_set_hooks(get, put)
//   Registers the two callbacks the embedded code reaches through the C
//   entry points factor_embed_map_get and factor_embed_map_put (which the
//   bridge's FFI prelude wraps as Factor words). Absent in an older fork.
typedef void (*factor_embed_map_set_hooks_fn)(factor_map_get_fn, factor_map_put_fn);

#endif
