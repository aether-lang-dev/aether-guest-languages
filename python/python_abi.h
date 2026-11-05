// python_abi.h: the part of CPython's C API that aether_host_python.c
// declares itself.
//
// The bridge loads libpython with dlopen and finds every function with
// dlsym, so it needs CPython's types and a few constants at compile time,
// never a symbol at link time. In aether's tree it took them from the
// system's Python.h (python3-dev), found by `make contrib`'s python3-config
// probe. Here the bridge is compiled into the program that imports it
// (`@source` in module.ae), where no such probe runs, so it carries the
// declarations it uses instead. A machine that runs a program built with it
// needs the Python runtime library only, not the dev package.
//
// What is declared, and why it is the same for every CPython 3.10 to 3.14
// with the default (GIL) build:
//
// - Objects, type objects, thread states and frames are only ever pointers
//   here: their layouts are never read. Reference counts go through the
//   exported functions Py_IncRef and Py_DecRef, never the Py_INCREF macros,
//   which read and write ob_refcnt inline, and whose layout and meaning
//   changed in 3.12 (immortal objects) and again in the free-threaded build.
// - The one struct the bridge fills in for CPython to read is PyMethodDef:
//   { const char* ml_name; PyCFunction ml_meth; int ml_flags;
//     const char* ml_doc; }, unchanged since Python 2. It is handed to
//   PyCFunction_NewEx, which makes one function object from it.
// - PyModuleDef is not used. It begins with PyModuleDef_HEAD_INIT, a
//   PyObject_HEAD whose initial values (an immortal refcount from 3.12) and
//   size (the free-threaded header) depend on the Python it was compiled
//   for. aether's copy built its `_aether_map` module from one; this one
//   makes the module with PyModule_New and sets each function on it.
// - The constants are part of the stable ABI: Py_file_input 257,
//   Py_eval_input 258, METH_VARARGS 1.
// - PyGILState_STATE is an enum (an int) of two values.
//
// The free-threaded build (3.13t, 3.14t: Py_GIL_DISABLED) has a different
// object header and reference counting. Nothing above reads either, but the
// bridge has not been run against one, and its GIL-based reasoning (one
// thread in the interpreter at a time) does not hold there. So the bridge
// refuses it at load: such a libpython exports _Py_DecRefShared, which the
// default build does not, and python.init() returns -1 saying so.
#ifndef AETHER_GUEST_PYTHON_ABI_H
#define AETHER_GUEST_PYTHON_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef ssize_t Py_ssize_t;

typedef struct _object PyObject;
typedef struct _typeobject PyTypeObject;
typedef struct _ts PyThreadState;
typedef struct _frame PyFrameObject;
typedef struct PyCompilerFlags PyCompilerFlags;   // only ever NULL here

typedef enum { PyGILState_LOCKED, PyGILState_UNLOCKED } PyGILState_STATE;

typedef PyObject* (*PyCFunction)(PyObject* self, PyObject* args);
typedef int (*Py_tracefunc)(PyObject* obj, PyFrameObject* frame, int what, PyObject* arg);
typedef void (*PyCapsule_Destructor)(PyObject* capsule);

typedef struct PyMethodDef {
    const char* ml_name;
    PyCFunction ml_meth;
    int ml_flags;
    const char* ml_doc;
} PyMethodDef;

#define METH_VARARGS 0x0001

#define Py_file_input 257
#define Py_eval_input 258

#endif
