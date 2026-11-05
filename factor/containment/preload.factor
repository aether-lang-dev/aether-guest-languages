! The vocabularies checks.factor uses, loaded by the host (host.ae) or the
! worker (worker.ae) before the sandbox is in force. They are all in the
! stock factor.image, but a USING: of one still reads the disk the first time
! it is parsed (a platforms.txt beside the vocabulary, for one), which a
! policy with no read grant on Factor's own directories refuses; once parsed,
! it reads nothing. Keep this list the same as checks.factor's.
USING: accessors alien alien.c-types alien.data alien.libraries alien.syntax arrays assocs
byte-arrays combinators continuations debugger environment formatting hashtables io
io.encodings.utf8 io.files io.launcher io.streams.string kernel libc locals math math.order math.parser
namespaces sequences splitting strings system unix vocabs words ;
