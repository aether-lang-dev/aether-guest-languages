#ifndef AETHER_HOST_PERL_H
#define AETHER_HOST_PERL_H

#include <stdint.h>

int aether_perl_run_sandboxed(void* perms, const char* code);
int aether_perl_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token);
int aether_perl_run(const char* code);
int aether_perl_init(void);
void aether_perl_finalize(void);

// Host functions a script calls (added here; see PROVENANCE.md).
const char* aether_perl_last_error(void);
int aether_perl_register(const char* name, void* fn);
int aether_perl_arg_count(void* ctx);
const char* aether_perl_arg_str(void* ctx, int i);
int64_t aether_perl_arg_int(void* ctx, int i);
void aether_perl_push_str(void* ctx, const char* v);
void aether_perl_push_int(void* ctx, int64_t v);
int aether_perl_raise_error(void* ctx, const char* msg);

#endif
