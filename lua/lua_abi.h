// lua_abi.h: the part of Lua's C API that aether_host_lua.c declares itself.
//
// The bridge loads liblua with dlopen and finds every function with dlsym,
// so it needs Lua's types and a few constants at compile time, never a
// symbol at link time. In aether's tree it took them from the system's
// lua.h (liblua5.x-dev), found by `make contrib`'s pkg-config probe. Here
// the bridge is compiled into the program that imports it (`@source` in
// module.ae), where no such probe runs, so it carries the declarations it
// uses instead. A machine that runs a program built with it needs the Lua
// runtime library only, not the dev package.
//
// These are Lua's own definitions under its default luaconf.h, which every
// distribution and Homebrew ship for 5.3, 5.4 and 5.5: lua_Integer is
// `long long`, lua_Number `double`, lua_KContext `intptr_t`. A liblua built
// with LUA_32BITS or a non-default LUA_INT_TYPE is not supported (nor was
// it with the system headers: they describe the build host's liblua, not
// the deploy host's, which is the one dlopen finds).
#ifndef AETHER_GUEST_LUA_ABI_H
#define AETHER_GUEST_LUA_ABI_H

#include <stddef.h>
#include <stdint.h>

typedef struct lua_State lua_State;
typedef struct lua_Debug lua_Debug;     // only ever a pointer here

typedef long long lua_Integer;
typedef double lua_Number;
typedef intptr_t lua_KContext;

typedef int (*lua_CFunction)(lua_State* L);
typedef int (*lua_KFunction)(lua_State* L, int status, lua_KContext ctx);
typedef void (*lua_Hook)(lua_State* L, lua_Debug* ar);

#define LUA_MULTRET (-1)
#define LUA_MASKCOUNT (1 << 3)

#endif
