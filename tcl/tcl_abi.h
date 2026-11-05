// tcl_abi.h: the part of Tcl's C API that aether_host_tcl.c declares itself.
//
// The bridge loads libtcl with dlopen and finds every function with dlsym,
// so it needs Tcl's types and a few constants at compile time, never a
// symbol at link time. In aether's tree it took them from the system's
// tcl.h (tcl-dev), found by `make contrib`'s pkg-config probe. Here the
// bridge is compiled into the program that imports it (`@source` in
// module.ae), where no such probe runs, so it carries the declarations it
// uses instead. A machine that runs a program built with it needs the Tcl
// runtime library only, not the dev package.
//
// Tcl's stubs mechanism (tclStubs, Tcl_InitStubs) is for extensions loaded
// into a Tcl that is already running; an embedder links libtcl or, as here,
// dlopens it and looks each function up with dlsym.
//
// What is declared, and why it serves Tcl 8.5, 8.6 and 9.0 alike:
//
// - Interpreters, commands and the clientData are only ever pointers.
// - Tcl_Obj is read in one place only: typePtr, to tell an integer from a
//   string (the typed readers of the two-way VM). Everything else about an
//   object goes through the exported functions, and the reference counts
//   through Tcl_DbIncrRefCount and Tcl_DbDecrRefCount, because the
//   Tcl_IncrRefCount macros write the refCount field, whose width Tcl 9
//   changed. In 8.5, 8.6 and 9.0 a Tcl_Obj begins refCount, bytes, length,
//   typePtr: refCount and length are int or Tcl_Size (ptrdiff_t), and
//   either way the three fields before typePtr take three pointers' worth
//   of space on LP64 and on ILP32, so typePtr is the fourth word. The
//   struct below is those four words and no more; it is only ever read
//   through a pointer Tcl gave. Tcl_ObjType's first field is its name in
//   all three.
// - Tcl 9 widened every length, count and index parameter from int to
//   Tcl_Size (ptrdiff_t). The prototypes of the dlsym'd functions here
//   declare them ptrdiff_t, whichever Tcl is loaded: an 8.x callee reads
//   the low 32 bits of the argument register, so the same call serves both
//   (x86-64 and arm64, and ILP32 where the two types are one). The one
//   place Tcl calls back with such a count, a command's objc, is received
//   as ptrdiff_t and truncated to int (see aether_host_tcl.c). The bridge
//   passes -1 (TCL_INDEX_NONE, "measure it") and small counts only.
// - The constants are Tcl's, unchanged from 8.5: TCL_OK 0, TCL_ERROR 1,
//   TCL_RETURN 2, TCL_BREAK 3, TCL_CONTINUE 4, TCL_GLOBAL_ONLY 1,
//   TCL_EVAL_GLOBAL 0x20000, TCL_LIMIT_COMMANDS 1, TCL_LIMIT_TIME 2.
// - Tcl_Time is { long sec; long usec; } in 8.x. Tcl 9 made sec a long long,
//   the same size as long where long is 64 bits; the bridge refuses Tcl 9
//   where it is not (aether_host_tcl.c).
// - Tcl_WideInt is long long in every Tcl 8.5 and later.
#ifndef AETHER_GUEST_TCL_ABI_H
#define AETHER_GUEST_TCL_ABI_H

#include <stddef.h>
#include <stdint.h>

typedef struct Tcl_Interp Tcl_Interp;       // opaque
typedef struct Tcl_Command_ *Tcl_Command;    // opaque
typedef void *ClientData;

typedef struct Tcl_ObjType {
    const char *name;
} Tcl_ObjType;

typedef struct Tcl_Obj {
    void *before_type[3];        // refCount, bytes, length (never touched)
    const Tcl_ObjType *typePtr;
} Tcl_Obj;

typedef long long Tcl_WideInt;
typedef ptrdiff_t Tcl_Size;      // see above: the width of Tcl 9's, used for 8.x calls too

typedef struct Tcl_Time {
    long sec;
    long usec;
} Tcl_Time;

// A command's implementation. Tcl 8.x passes objc as an int, Tcl 9 as a
// Tcl_Size: the bridge's command procedure takes the wide type and keeps the
// low 32 bits.
typedef int (Tcl_ObjCmdProc)(ClientData clientData, Tcl_Interp *interp,
                             Tcl_Size objc, Tcl_Obj *const objv[]);
typedef void (Tcl_CmdDeleteProc)(ClientData clientData);

#define TCL_OK 0
#define TCL_ERROR 1
#define TCL_RETURN 2
#define TCL_BREAK 3
#define TCL_CONTINUE 4

#define TCL_GLOBAL_ONLY 1
#define TCL_EVAL_GLOBAL 0x20000

#define TCL_LIMIT_COMMANDS 1
#define TCL_LIMIT_TIME 2

#endif
