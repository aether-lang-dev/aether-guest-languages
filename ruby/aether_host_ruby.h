#ifndef AETHER_HOST_RUBY_H
#define AETHER_HOST_RUBY_H

#include <stdint.h>

// Tier 1: fire-and-forget (as in aether's contrib/host/ruby)
int ruby_run_sandboxed(void* perms, const char* code);
int ruby_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token);
int ruby_run(const char* code);
int ruby_init_host(void);
void ruby_finalize_host(void);

// Tier 2: host functions and typed values (new here; python's and lua's
// shape). The interpreter is one per process, so there is no VM handle: the
// `ctx` a callback receives is its call.
int ruby_register(const char* name, void* host_fn);
int ruby_eval(const char* code);
const char* ruby_result_str(void);
const char* ruby_set_instruction_limit(int64_t budget);

// Callback side: the arguments, 1-based
int ruby_arg_count(void* ctx);
int ruby_arg_type(void* ctx, int i);
const char* ruby_arg_str(void* ctx, int i);
int64_t ruby_arg_int(void* ctx, int i);
double ruby_arg_double(void* ctx, int i);
int ruby_arg_bool(void* ctx, int i);

// Callback side: the value it returns (the last push wins; return 1 to
// return it, 0 for nil)
void ruby_push_int(void* ctx, int64_t v);
void ruby_push_str(void* ctx, const char* v);
void ruby_push_bool(void* ctx, int v);
void ruby_push_nil(void* ctx);
void ruby_push_double(void* ctx, double v);
int ruby_raise_error(void* ctx, const char* msg);

#endif
