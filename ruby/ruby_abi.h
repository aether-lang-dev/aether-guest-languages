/* ruby_abi.h: the little of CRuby's C API the bridge uses, declared here so
 * that building it needs no Ruby dev package (no ruby.h, no libruby to link).
 * The bridge finds every function it calls with dlsym in the libruby it
 * dlopens at run time (aether_host_ruby.c, "libruby dlopen table"), so what
 * has to be right is only the shapes below, and they are the same from
 * Ruby 2.6 to 3.4 and 4.0 on 64-bit POSIX:
 *
 *   VALUE and ID  are pointer-sized unsigned integers. (VALUE is `unsigned
 *                 long` and ID `uintptr_t` in ruby.h; both are 64 bits
 *                 wherever uintptr_t is.)
 *
 *   Qnil, Qtrue, Qfalse  are NOT declared, and nothing here hard-codes
 *                 them. They are compile-time enum constants in ruby.h, not
 *                 exported symbols, and they differ between Rubies: Qnil is
 *                 0x08 from 2.0 to 3.3 and 0x04 from 3.4 (aether's own copy
 *                 of this bridge baked in 0x08 and crashed on 3.4, #1618),
 *                 and the whole special-constant encoding depends on
 *                 USE_FLONUM. The bridge asks the loaded interpreter at
 *                 init: it evaluates `nil`, `true` and `false` once and
 *                 keeps the VALUEs it gets back. Truthiness (RTEST) is then
 *                 "neither Qnil nor Qfalse", Qundef is never needed, and no
 *                 type is read from a VALUE's bits: a value's kind is asked
 *                 of the interpreter with rb_obj_class.
 *
 *   rb_cInteger, rb_cString, rb_cFloat, rb_eRuntimeError  are exported data
 *                 symbols (each a VALUE); dlsym gives the address of the
 *                 variable, which the bridge reads.
 *
 * Functions with fixed-arity callbacks (rb_define_global_function with a
 * count of 1 or 2) take the receiver first; with -1 the callback is
 * (int argc, VALUE* argv, VALUE self). Both are what ruby.h's ANYARGS cast
 * hides.
 */
#ifndef AETHER_RUBY_ABI_H
#define AETHER_RUBY_ABI_H

#include <stdint.h>

typedef uintptr_t VALUE;
typedef uintptr_t ID;

/* A C function Ruby calls as a method: (int argc, VALUE* argv, VALUE self)
 * for a count of -1, (VALUE self, VALUE a [, VALUE b]) for 1 and 2. */
typedef VALUE (*ruby_cfunc_varargs)(int argc, VALUE* argv, VALUE self);
typedef VALUE (*ruby_cfunc_1)(VALUE self, VALUE a);
typedef VALUE (*ruby_cfunc_2)(VALUE self, VALUE a, VALUE b);

#endif
