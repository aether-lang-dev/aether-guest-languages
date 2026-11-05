#ifndef AETHER_HOST_JAVA_H
#define AETHER_HOST_JAVA_H

#include <stdint.h>

int java_run_sandboxed(void* perms, const char* classpath, const char* main_class);
int java_run(const char* classpath, const char* main_class);
int java_run_sandboxed_with_map(void* perms, const char* classpath, const char* main_class, uint64_t map_token);

#endif
