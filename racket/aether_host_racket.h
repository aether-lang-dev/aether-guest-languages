#ifndef AETHER_HOST_RACKET_H
#define AETHER_HOST_RACKET_H

#include <stdint.h>

#include "aether_string.h"

// One embedded Racket CS VM, shared by the racket_* and rhombus_* calls: Rhombus
// is a #lang on the Racket runtime. `lang` selects how source text is read
// before it is evaluated: 0 = Racket s-expressions, 1 = a `#lang rhombus` module.
// The result-returning call is named `evaluate` (as in aether's copy of this
// bridge), not `eval`.

// Fire-and-forget: evaluate for effect; output goes to the process's stdout.
// 0 on success, -1 on an error (printed to stderr, kept for racket_last_error).
int racket_run(const char* code);
int racket_run_sandboxed(void* perms, const char* code);
int racket_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token);
// Evaluate `code` and return what it printed ("" if the VM cannot load; on an
// error the output so far and "error: <message>"). The caller owns the string.
AetherString* racket_evaluate(const char* code);

int racket_set(const char* key, const char* value);
AetherString* racket_get(const char* key);
int racket_init(void);
void racket_finalize(void);
const char* racket_last_error(void);

// Two-way: register a host function `int fn(void* vm)` as a Racket procedure.
int racket_register(const char* name, void* host_fn);
int racket_arg_count(void* vm);
int racket_arg_type(void* vm, int i);
const char* racket_arg_str(void* vm, int i);
int64_t racket_arg_int(void* vm, int i);
int racket_arg_bool(void* vm, int i);
void racket_push_int(void* vm, int64_t v);
void racket_push_str(void* vm, const char* v);
void racket_push_bool(void* vm, int v);
void racket_push_nil(void* vm);
int racket_raise_error(void* vm, const char* msg);

int rhombus_run(const char* code);
int rhombus_run_sandboxed(void* perms, const char* code);
int rhombus_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token);
AetherString* rhombus_evaluate(const char* code);
int rhombus_set(const char* key, const char* value);
AetherString* rhombus_get(const char* key);
int rhombus_init(void);
void rhombus_finalize(void);

#endif
