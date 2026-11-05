// perl_abi.h: the part of Perl's C API that aether_host_perl.c declares itself.
//
// The bridge loads libperl with dlopen and finds every function with dlsym,
// so it needs Perl's types and a few constants at compile time, never a
// symbol at link time. In aether's tree it took them from the system's
// <EXTERN.h> and <perl.h> (the perl dev headers, libperl-dev on Debian),
// found by `make contrib`'s probe. Here the bridge is compiled into the
// program that imports it (`@source` in module.ae), where no such probe
// runs, so it carries the declarations it uses instead. A machine that runs
// a program built with it needs the Perl runtime library only, not the dev
// headers.
//
// What is declared, and why it is the same for every Perl 5.30 and later
// that has a multi-interpreter build (MULTIPLICITY, which every system perl
// on Linux, macOS and the BSDs has: threaded or not):
//
// - The interpreter, SVs, AVs, CVs and MAGIC are only ever pointers here:
//   their layouts are never read. Every access goes through an exported
//   Perl_* function. No SvPV/SvTRUE/ERRSV/dXSARGS macro is used, since they
//   read interpreter and SV fields inline.
//
// - THE CONTEXT ARGUMENT. In a MULTIPLICITY build every API function takes
//   the interpreter as its first argument (pTHX_). The bridge passes the
//   interpreter it made (`my_perl`) explicitly. In a build without
//   MULTIPLICITY the same functions take no such argument, and the
//   interpreter's variables are plain globals: that build exports data
//   symbols such as PL_errgv, which a MULTIPLICITY build (where they are
//   fields of the interpreter structure) does not. The bridge looks for
//   PL_errgv at load and refuses such a libperl with a message, rather than
//   call functions with the wrong arguments.
//
// - THREADS. A threaded perl (ithreads) keeps "the current interpreter" in a
//   thread-local key, which some of perl's own code reads (dTHX). A thread
//   other than the one that made the interpreter has to select it first:
//   Perl_set_context(my_perl), looked up if the library exports it (every
//   threaded build does). The bridge calls it on entry and serialises
//   entries with a lock: one interpreter, one thread in it at a time.
//
// - MGVTBL is the one structure the bridge fills in for perl to read. It is
//   documented in perlguts ("Magic Virtual Tables"): eight function
//   pointers (get, set, len, clear, free, copy, dup, local), in that order
//   since 5.8. The bridge uses only `get`, to run C when a scalar is read:
//   that is how a Perl sub calls the host (see aether_host_perl.c), without
//   the XSUB argument-stack macros, which would need the interpreter's
//   layout.
//
// - Constants: SV_GMAGIC 2 (sv.h), GV_ADD 1 (gv.h), PERL_MAGIC_ext '~'.
//   All three have had these values since 5.8 and are part of the public
//   API, as is the (pTHX_ SV*, MAGIC*) shape of a get hook.
#ifndef AETHER_GUEST_PERL_ABI_H
#define AETHER_GUEST_PERL_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct interpreter PerlInterpreter;   // opaque
typedef struct sv SV;                         // opaque
typedef struct cv CV;                         // opaque
typedef struct magic MAGIC;                   // opaque

typedef int32_t I32;
typedef uint32_t U32;
typedef size_t STRLEN;

typedef void (*XSINIT_t)(PerlInterpreter*);
typedef void (*XSUBADDR_t)(PerlInterpreter*, CV*);

typedef struct mgvtbl {
    int (*svt_get)(PerlInterpreter*, SV*, MAGIC*);
    int (*svt_set)(PerlInterpreter*, SV*, MAGIC*);
    U32 (*svt_len)(PerlInterpreter*, SV*, MAGIC*);
    int (*svt_clear)(PerlInterpreter*, SV*, MAGIC*);
    int (*svt_free)(PerlInterpreter*, SV*, MAGIC*);
    int (*svt_copy)(PerlInterpreter*, SV*, MAGIC*, SV*, const char*, I32);
    int (*svt_dup)(PerlInterpreter*, MAGIC*, void*);
    int (*svt_local)(PerlInterpreter*, SV*, MAGIC*);
} MGVTBL;

#define SV_GMAGIC 2
#define GV_ADD 1
#define PERL_MAGIC_ext '~'

#endif
