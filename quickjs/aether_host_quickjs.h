#ifndef AETHER_HOST_QUICKJS_H
#define AETHER_HOST_QUICKJS_H

#include <stdint.h>

// The VM (module.ae has the API and the ownership rule of each call).
void* qjsb_new(int memory_kb);
void* qjsb_engine(void* vm);
void quickjs_vm_free(void* vm);
int quickjs_vm_busy(void* vm);
int quickjs_vm_eval(void* vm, const void* code);
int quickjs_vm_result_type(void* vm);
int64_t quickjs_vm_result_int(void* vm);
double quickjs_vm_result_double(void* vm);
const void* quickjs_vm_result_str(void* vm);
int quickjs_vm_result_bool(void* vm);
void* quickjs_vm_result_bytes(void* vm);
const void* quickjs_vm_result_json(void* vm);
void quickjs_vm_pop_result(void* vm);
const char* quickjs_vm_set_global_str(void* vm, const void* name, const void* value);
const char* quickjs_vm_set_global_int(void* vm, const void* name, int64_t value);
const char* quickjs_vm_set_global_strlist(void* vm, const void* name, void* items);
const char* quickjs_vm_set_global_json(void* vm, const void* name, const void* json);
const char* quickjs_vm_set_time_limit(void* vm, int64_t ms, int every);
const char* quickjs_vm_set_instruction_limit(void* vm, int64_t budget, int every);
void quickjs_vm_set_cancel_cell(void* vm, void* cell);
void quickjs_vm_set_user(void* vm, void* p);
void* quickjs_vm_user(void* vm);

// Registration (module.ae's vm_register makes the closure; these do the rest).
int qjsb_name_ok(const void* name);
const void* qjsb_leaf(const void* name);
int qjsb_install(void* vm, const void* name, int f);
int qjsb_dispatch(void* vm, void* host_fn, int this_h, int args);

// The callback side.
int quickjs_arg_count(void* vm);
int quickjs_arg_type(void* vm, int i);
const void* quickjs_arg_str(void* vm, int i);
int64_t quickjs_arg_int(void* vm, int i);
double quickjs_arg_double(void* vm, int i);
int quickjs_arg_bool(void* vm, int i);
void* quickjs_arg_bytes(void* vm, int i);
const void* quickjs_arg_json(void* vm, int i);
void quickjs_push_int(void* vm, int64_t v);
void quickjs_push_str(void* vm, const void* v);
void quickjs_push_bool(void* vm, int v);
void quickjs_push_nil(void* vm);
void quickjs_push_double(void* vm, double v);
void quickjs_push_bytes(void* vm, void* b);
int quickjs_push_json(void* vm, const void* json);
int quickjs_raise_error(void* vm, const void* msg);

// The bridge's own host functions and run's helpers.
void* qjsb_print_fn(void);
void* qjsb_map_get_fn(void);
void* qjsb_map_put_fn(void);
void qjsb_set_map(void* vm, uint64_t token);
int qjsb_push_grants(void* perms);
void qjsb_pop_grants(void);
void qjsb_report(void* vm);

int quickjs_init(void);
void quickjs_finalize(void);

#endif
