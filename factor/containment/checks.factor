! The rows of stunt's catalogue (aether contrib/host/stunt) that Factor can
! express, each done the way a Factor program would do it, and the judge that
! turns what happened into stunt's one line:
!
!   <check>: <outcome> (<detail>) - expected <expectation>: PASS|FAIL
!
! The host (host.ae, or child_host.ae for the contained child) puts a few
! facts it found before the sandbox was in force in front of this file
! (HOST_HOME_SET, HOST_SH, LIBM_PATH, LIBC_NAME, LIBC_PATH: policy/module.ae)
! and appends a call of check-line. A refusal is what a sandbox gives: EPERM
! or EACCES from libc, the preload library or the kernel, or a host function's
! answer that starts "refused:". Anything else that goes wrong is an error,
! never a refusal, so it cannot pass for a sandbox at work.
!
! Every vocabulary used here is in the stock factor.image, so a parse never
! reads the disk: under a policy with no read grant on Factor's own
! directories, a vocabulary that is not in the image could not be loaded
! (factor/README.md, "Loading libfactor and the image").
!
! Factor has two ways to reach libc: its own vocabularies (io.files,
! io.launcher, libc, environment), and the FFI, which can name any function
! of the process. Both are used, as a Factor program would use them. The FFI
! words below resolve in the process's own symbol scope, not in a handle on
! libc, so the preload's wrappers are what they find.

! (preload.factor loads the same vocabularies before the sandbox.)
USING: accessors alien alien.c-types alien.data alien.libraries alien.syntax arrays assocs
byte-arrays combinators continuations debugger environment formatting hashtables io
io.encodings.utf8 io.files io.launcher io.streams.string kernel libc locals math math.order math.parser
namespaces sequences splitting strings system unix vocabs words ;
IN: stunt

FUNCTION-ALIAS: c-getenv c-string getenv ( c-string name )
FUNCTION-ALIAS: c-system int system ( c-string command )
FUNCTION-ALIAS: c-fork int fork ( )
FUNCTION-ALIAS: c-exit void _exit ( int status )
FUNCTION-ALIAS: c-waitpid int waitpid ( int pid, void* status, int options )
FUNCTION-ALIAS: c-socket int socket ( int domain, int type, int protocol )
FUNCTION-ALIAS: c-fcntl int fcntl ( int fd, int cmd, long arg )
FUNCTION-ALIAS: c-connect int connect ( int fd, void* addr, int len )
FUNCTION-ALIAS: c-close int close ( int fd )
FUNCTION-ALIAS: c-dlerror c-string dlerror ( )
FUNCTION-ALIAS: c-syscall long syscall ( long nr, long a, c-string b, long c, long d )
FUNCTION-ALIAS: c-memfd-create int memfd_create ( c-string name, uint flags )
FUNCTION-ALIAS: c-ftruncate int ftruncate ( int fd, long length )
FUNCTION-ALIAS: c-mmap void* mmap ( void* addr, size_t length, int prot, int flags, int fd, long offset )
FUNCTION-ALIAS: c-memcpy void* memcpy ( void* dest, void* src, size_t n )
FUNCTION-ALIAS: c-write long write ( int fd, void* buf, size_t n )
FUNCTION-ALIAS: c-sleep uint sleep ( uint seconds )

! The embedding's one channel to the host: the two entry points libfactor
! exports for the key-value hooks (the fork's vm/embed_api.cpp), which the
! bridge's own aether-map-get and aether-map-put wrap.
LIBRARY: factor
FUNCTION-ALIAS: c-hook-get c-string factor_embed_map_get ( c-string key )

! ---- outcomes ---------------------------------------------------------------

! A Factor error as one line of text.
: error>text ( err -- s )
    [ error. ] with-string-writer "\n" " " replace 120 over length min head ;

: error-errno ( error -- n/f )
    {
        { [ dup unix-system-call-error? ] [ errno>> ] }
        { [ dup libc-error? ] [ errno>> ] }
        [ drop f ]
    } cond ;

! EPERM and EACCES are the same numbers on Linux, macOS and FreeBSD.
:: by-errno ( code what -- outcome detail )
    code { 1 13 } member? "refused" "error" ?
    what code strerror code "%s: %s (errno %d)" sprintf ;

! A Factor error, with the errno when it is a system call's.
:: failed ( err what -- outcome detail )
    err error-errno :> code
    code [ code what by-errno ] [ "error" what err error>text "%s: %s" sprintf ] if ;

! The first line of a text, as stunt shows it.
: first-line ( s -- s' )
    "\n" split1 drop dup length 40 min head ;

! ---- open, write ------------------------------------------------------------

! utf8 file-contents: Factor's way to a file, through libc's open.
:: open-path ( path -- outcome detail )
    [ path utf8 file-contents first-line "read \"%s\"" sprintf "succeeded" swap ]
    [ "io.files" failed ] recover ;

:: write-file ( -- outcome detail )
    [ "written by factor\n" "./stunt-written.txt" utf8 set-file-contents
      "succeeded" "wrote ./stunt-written.txt" ]
    [ "io.files" failed ] recover ;

! ---- environment ------------------------------------------------------------

! libc's getenv, through the FFI: os-env in Factor is getenv too, but
! reached through the vocabulary; this is the FFI's way, as the rows name it.
: libc-getenv ( -- outcome detail )
    "HOME" c-getenv [ "HOME=%s" sprintf "succeeded" swap ] [
        HOST_HOME_SET
        [ "refused" "libc getenv gave NULL; HOME is in the environment" ]
        [ "error" "HOME is unset" ] if
    ] if* ;

! os-envs: the whole environment, read from the process's environ array
! rather than asked of getenv, so the preload's getenv does not see it. A
! missing HOME is a refusal only when the host had one (the contained child
! is given no environment).
: env-cached ( -- outcome detail )
    os-envs "HOME" of [ "HOME=%s from os-envs" sprintf "succeeded" swap ] [
        HOST_HOME_SET
        [ "refused" "HOME is not in os-envs; the host has it" ]
        [ "error" "HOME was never in the environment" ] if
    ] if* ;

! ---- sockets ----------------------------------------------------------------

! A non-blocking connect, judged as stunt judges it: EACCES at connect (the
! preload, Landlock) or EPERM/EACCES at socket (seccomp) is a refusal; EPERM
! at connect is a firewall's answer, an error; anything else (EINPROGRESS, a
! network error) means the call was let through. The sockaddr is built by
! hand for Linux's layout: family, port 443, the address.
:: connect-to ( family addr -- outcome detail )
    family 1 0 c-socket :> fd
    fd 0 < [ errno "socket" by-errno ] [
        fd 4 2048 c-fcntl drop
        fd addr addr length c-connect :> r
        errno :> e
        fd c-close drop
        {
            { [ r 0 = ] [ "succeeded" "connected" ] }
            { [ e 13 = ] [ "refused" e strerror "connect: %s" sprintf ] }
            { [ e 1 = ] [ "error" e strerror "connect: %s (a firewall's answer, not the sandbox's)" sprintf ] }
            [ "succeeded" e strerror "let through: %s" sprintf ]
        } cond
    ] if ;

CONSTANT: sockaddr-192.0.2.1 B{ 2 0 1 187 192 0 2 1 0 0 0 0 0 0 0 0 }
CONSTANT: sockaddr-ipv4-mapped B{ 10 0 1 187 0 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 192 0 2 1 0 0 0 0 }

! ---- processes --------------------------------------------------------------

! Starting /bin/sh, both ways Factor has: libc's system() through the FFI
! (glibc starts the shell with posix_spawn), and io.launcher's run-process
! (fork and exec in Factor's own VM code). Succeeded when either started a
! shell: an escape by one way is an escape. Refused when both were refused: a
! system call error of EPERM or EACCES, or the shell could not be executed
! (exit 127) while HOST_SH says /bin/sh is there.
: exited-127? ( status -- ? ) -8 shift 255 bitand 127 = ;

:: exec-system ( -- kind detail )
    "exit 0" c-system :> st
    {
        { [ st 0 = ] [ "succeeded" "system: exit 0" ] }
        { [ st -1 = ] [ "refused" "system: system() returned -1" ] }
        { [ st exited-127? HOST_SH and ] [ "refused" "system: the shell could not be executed (exit 127)" ] }
        [ "error" st "system: status %d" sprintf ]
    } cond ;

:: exec-launcher ( -- kind detail )
    [
        { "/bin/sh" "-c" "exit 0" } run-process wait-for-process :> st
        {
            { [ st 0 = ] [ "succeeded" "run-process: exit 0" ] }
            { [ st 127 = HOST_SH and ] [ "refused" "run-process: the shell could not be executed (exit 127)" ] }
            [ "error" st "run-process: exit %d" sprintf ]
        } cond
    ] [ "run-process" failed ] recover ;

:: exec-sh ( -- outcome detail )
    exec-system :> ( k1 d1 )
    exec-launcher :> ( k2 d2 )
    d1 d2 "%s; %s" sprintf :> detail
    {
        { [ k1 "succeeded" = k2 "succeeded" = or ] [ "succeeded" detail ] }
        { [ k1 "refused" = k2 "refused" = and ] [ "refused" detail ] }
        [ "error" detail ]
    } cond ;

! fork, the child exiting at once: 0 succeeded, anything else an error.
:: fork-once ( -- outcome detail )
    c-fork :> pid
    {
        { [ pid 0 = ] [ 0 c-exit "error" "the forked child went on" ] }
        { [ pid 0 < ] [ errno "fork" by-errno ] }
        [
            0 int <ref> :> st
            pid st 0 c-waitpid drop
            st int deref :> s
            s 127 bitand 0 = s -8 shift 255 bitand 0 = and
            [ "succeeded" "the forked child ran" ]
            [ "error" s "the forked child: status %d" sprintf ] if
        ]
    } cond ;

! ---- dlopen -----------------------------------------------------------------

! dlopen, Factor's way (what DLL" " and add-library do). A dlopen that fails
! on its own leaves a dlerror() message; the preload's refusal returns NULL
! without one, so no message, or a "Permission denied" or "Operation not
! permitted" in it, is the refusal.
:: load-library ( path -- outcome detail )
    [ path dlopen dll-valid? ] [ drop f ] recover :> ok
    ok [ "succeeded" path "loaded %s" sprintf ] [
        c-dlerror :> msg
        {
            { [ msg not ] [ "refused" "dlopen failed with no dlerror() message (the preload's refusal)" ] }
            { [ msg "Permission denied" subseq? msg "Operation not permitted" subseq? or ]
              [ "refused" msg "dlopen: %s" sprintf ] }
            [ "error" msg "dlopen: %s" sprintf ]
        } cond
    ] if ;

:: need ( value what -- value )
    value empty? [ what "the host found no %s" sprintf throw ] when value ;

! ---- system calls past libc -------------------------------------------------

: arch-openat ( -- n ) cpu x86.64 = [ 257 ] [ 56 ] if ;
: arch-socket ( -- n ) cpu x86.64 = [ 41 ] [ 198 ] if ;

! libc's syscall(): only EPERM or EACCES is a refusal (ENOSYS is the
! preload's syscall() with no grants in force, which is an error).
:: syscall-fn-openat ( -- outcome detail )
    arch-openat -100 "/etc/passwd" 0 0 c-syscall :> r
    errno :> e
    r 0 >= [ r c-close drop "succeeded" "opened /etc/passwd" ]
    [ e "syscall()" by-errno ] if ;

! The syscall instruction itself, past libc and so past any preload: a few
! bytes of machine code in an executable mapping, called through the FFI
! (alien-indirect). raw(nr, a, b, c, d) moves its arguments into the system
! call registers and traps; the result is the kernel's, -errno on failure.
!
! The mapping is of a memfd, not anonymous memory: the preload refuses an
! anonymous executable mmap and any mprotect to PROT_EXEC, but lets an
! executable mapping of a file descriptor through (so the loader and JITs can
! map code), and a memfd is one. So the script gets its system call
! instruction, and only the kernel (Landlock, seccomp) is left to hold what it
! does.
CONSTANT: raw-x86_64 B{ 72 137 248 72 137 247 72 137 214 72 137 202 77 137 194 15 5 195 }
CONSTANT: raw-aarch64 B{ 232 3 0 170 224 3 1 170 225 3 2 170 226 3 3 170 227 3 4 170 1 0 0 212 192 3 95 214 }

: raw-code ( -- bytes ) cpu x86.64 = [ raw-x86_64 ] [ raw-aarch64 ] if ;

:: make-raw ( -- addr )
    "raw" 0 c-memfd-create :> fd
    fd 0 < [ "memfd_create failed" throw ] when
    fd 4096 c-ftruncate drop
    f 4096 7 1 fd 0 c-mmap :> addr
    fd c-close drop
    addr alien-address 0xffffffffffffffff = [ "mmap failed" throw ] when
    addr raw-code dup length c-memcpy drop
    addr ;

: raw-call-path ( nr a path c d addr -- r )
    long { long long c-string long long } cdecl alien-indirect ;

: raw-call ( nr a b c d addr -- r )
    long { long long long long long } cdecl alien-indirect ;

:: raw-result ( r what -- outcome detail )
    r 0 >= [ r c-close drop "succeeded" what ]
    [ r neg "the syscall instruction" by-errno ] if ;

:: syscall-openat ( -- outcome detail )
    make-raw :> addr
    arch-openat -100 "/etc/passwd" 0 0 addr raw-call-path
    "the syscall instruction opened /etc/passwd" raw-result ;

:: syscall-socket ( -- outcome detail )
    make-raw :> addr
    arch-socket 2 1 0 0 addr raw-call
    "the syscall instruction made an AF_INET socket" raw-result ;

! ---- the host's functions ---------------------------------------------------

! The bridge has no host-function API, only the key-value hooks: the shared
! map. Over them a host can answer a call of its own, as these rows do: a key
! of the form ^A name ^A json-args asks the hook for the host function `name`
! and is answered "KIND ^A text", KIND being VALUE (text is the JSON value),
! FAIL (the message) or CANCEL. This is the contained child's std.guest
! protocol, as lua/python/ruby see it; in-process the host (host.ae) answers
! in C. aether-map-get and aether-map-put of the shared map are keys without
! the ^A.

! JSON, the little of it a host function's arguments and answers need.
SYMBOL: json-null
SYMBOL: jtext
SYMBOL: jpos

: json-quote ( s -- s' )
    [
        {
            { CHAR: " [ "\\\"" ] }
            { CHAR: \\ [ "\\\\" ] }
            { CHAR: \n [ "\\n" ] }
            [ dup 32 < [ "\\u%04x" sprintf ] [ 1string ] if ]
        } case
    ] { } map-as concat "\"" dup surround ;

: json-encode ( obj -- s )
    dup string? [ json-quote ] [ number>string ] if ;

: json-args ( seq -- s ) [ json-encode ] map "," join "[" "]" surround ;

: jpeek ( -- ch/f ) jpos get jtext get ?nth ;
: jnext ( -- ch )
    jpeek [ jpos [ 1 + ] change ] [ "unterminated json" throw ] if* ;
: jskip ( -- ) [ jpeek { 32 9 10 13 } member? ] [ jpos [ 1 + ] change ] while ;
: jexpect ( ch -- ) jnext = [ "bad json" throw ] unless ;

: jescape ( ch -- ch' )
    {
        { CHAR: n [ 10 ] }
        { CHAR: t [ 9 ] }
        { CHAR: r [ 13 ] }
        { CHAR: b [ 8 ] }
        { CHAR: f [ 12 ] }
        { CHAR: u [ 4 [ jnext ] replicate >string 16 base> ] }
        [ ]
    } case ;

:: jstring ( -- str )
    CHAR: " jexpect
    V{ } clone :> out
    [
        jnext :> c
        c CHAR: " = [ f ] [
            c CHAR: \\ = [ jnext jescape ] [ c ] if out push t
        ] if
    ] loop
    out >string ;

: jnumber ( -- n )
    V{ } clone
    [ jpeek dup [ "-+.eE0123456789" member? ] [ drop f ] if ]
    [ jnext over push ] while
    >string string>number ;

DEFER: jvalue

:: jarray ( -- seq )
    CHAR: [ jexpect V{ } clone :> out
    jskip jpeek CHAR: ] = [ jnext drop ] [
        [ jvalue out push jskip jnext CHAR: , = ] loop
    ] if
    out ;

:: jobject ( -- assoc )
    CHAR: { jexpect H{ } clone :> out
    jskip jpeek CHAR: } = [ jnext drop ] [
        [ jskip jstring :> k jskip CHAR: : jexpect jvalue k out set-at jskip jnext CHAR: , = ] loop
    ] if
    out ;

: jword ( n value -- value ) swap [ jnext drop ] times ;

: jvalue ( -- value )
    jskip jpeek {
        { CHAR: " [ jstring ] }
        { CHAR: { [ jobject ] }
        { CHAR: [ [ jarray ] }
        { CHAR: t [ 4 t jword ] }
        { CHAR: f [ 5 f jword ] }
        { CHAR: n [ 4 json-null jword ] }
        [ drop jnumber ]
    } case ;

: json-decode ( s -- value ) jtext set 0 jpos set jvalue ;

: sep ( -- s ) 1 1string ;

! host-call ( name args -- kind text ): the call, and the host's answer.
:: host-call ( name args -- kind text )
    sep name sep args 4array concat c-hook-get :> reply
    reply [ sep split1 ] [ "FAIL" "the host answered nothing" ] if* ;

! A host function's answer: a value, or why not. In-process a refusal is a
! FAIL that starts "refused:"; in the contained child a file comes as
! std.guest's answer to the call, {"granted": true, "text": ...} or
! {"granted": false, "why": ...}, and a FAIL is the message.
:: host-str ( fn arg -- outcome detail )
    fn { arg } json-args host-call :> ( kind text )
    kind "VALUE" = [
        text json-decode :> v
        v hashtable? [
            "granted" v at
            [ "text" v at first-line :> t "succeeded" fn arg t "%s(%s) = \"%s\"" sprintf ]
            [ "refused" "why" v at "refused: %s" sprintf ] if
        ] [
            v string? [ "succeeded" fn arg v first-line "%s(%s) = \"%s\"" sprintf ]
            [ "error" fn " answered nothing" append ] if
        ] if
    ] [
        text "refused:" head? [ "refused" text ] [ "error" text ] if
    ] if ;

:: host-value ( name args -- value/f )
    name args host-call :> ( kind text )
    kind "VALUE" = [ text json-decode ] [ f ] if ;

: callback-greet ( -- outcome detail )
    "greet" { "stunt" } json-args host-value
    dup "hello, stunt" = [ drop "succeeded" "greet(\"stunt\") = \"hello, stunt\"" ]
    [ "greet(\"stunt\") = %u" sprintf "error" swap ] if ;

: callback-add ( -- outcome detail )
    "add" { 2 40 } json-args host-value
    dup 42 = [ drop "succeeded" "add(2, 40) = 42" ]
    [ "add(2, 40) = %u" sprintf "error" swap ] if ;

! A host function nobody mounted is answered with a FAIL that names it, not
! a crash.
:: callback-unregistered ( -- outcome detail )
    "nope" { } json-args host-call :> ( kind text )
    kind "FAIL" = text "no host function nope" subseq? and
    [ "refused" text "no host function nope (%s)" sprintf ]
    [ "error" text ] if ;

! ---- the shared map ---------------------------------------------------------

! In-process the bridge's own words (vocabulary aether.host.map, its FFI
! prelude); in the contained child the host's map, through the same hooks.
: bridge-word ( name -- word/f ) "aether.host.map" lookup-word ;

: map-get ( key -- value/f )
    "aether-map-get" bridge-word
    [ execute( key -- value ) ]
    [ 1array json-args "aether_map_get" swap host-value dup json-null = [ drop f ] when ] if* ;

: map-put ( key value -- )
    "aether-map-put" bridge-word
    [ execute( key value -- ) ]
    [ 2array json-args "aether_map_put" swap host-value drop ] if* ;

: map-get-row ( -- outcome detail )
    "input" map-get
    {
        { [ dup not ] [ drop "error" "input is not in the map" ] }
        { [ dup "stunt-in" = not ] [ "input = %u" sprintf "error" swap ] }
        [ drop "succeeded" "input = \"stunt-in\"" ]
    } cond ;

: map-set-row ( -- outcome detail )
    "output" "from-factor" map-put
    "output" map-get
    dup "from-factor" = [ drop "succeeded" "output = \"from-factor\"" ]
    [ "output read back as %u" sprintf "error" swap ] if ;

! aether-map-put gives Factor no result, so a refused write is seen only by
! reading the key back: still the host's value.
: map-overwrite-input ( -- outcome detail )
    "input" "overwritten" map-put
    "input" map-get
    dup "stunt-in" = [ drop "refused" "input is still \"stunt-in\" (aether-map-put gives Factor no result)" ]
    [ "input read back as %u" sprintf "succeeded" swap ] if ;

! ---- host-checked rows, in the contained child -----------------------------

! Not answering a CANCEL: Factor has no way to be interrupted while it runs a
! word (no hook, no trace function, and its green threads are cooperative),
! so a loop ends only when the host kills the worker. The host judges how it
! ended.
: silent-loop ( -- ) [ t ] loop ;

! One 1 MiB print is one SAY frame in the contained child, past the host's
! max_frame: the host refuses it by its header.
: oversize ( -- ) 1048576 CHAR: x <string> print ;

! The worker's socket to the host is in its environment (std.ipc's
! AETHER_IPC_FD), which os-envs reads; c-write reaches it: a frame's 4-byte
! header saying 64 bytes, 10 of them, then nothing.
: half-frame ( -- )
    os-envs "AETHER_IPC_FD" of string>number
    B{ 0 0 0 64 67 65 76 76 32 103 114 101 101 116 }
    dup length c-write drop 30 c-sleep drop ;

! ---- the table --------------------------------------------------------------

: checks ( -- assoc )
    H{
        { "open-etc-passwd" [ "/etc/passwd" open-path ] }
        { "open-granted-file" [ "./stunt-fixture.txt" open-path ] }
        { "write-granted-file" [ write-file ] }
        { "getenv-home" [ libc-getenv ] }
        { "env-cached" [ env-cached ] }
        { "connect-192.0.2.1" [ 2 sockaddr-192.0.2.1 connect-to ] }
        { "connect-ipv4-mapped" [ 10 sockaddr-ipv4-mapped connect-to ] }
        { "exec-bin-sh" [ exec-sh ] }
        { "fork" [ fork-once ] }
        { "dlopen-libm" [ LIBM_PATH "libm" need load-library ] }
        { "dlopen-libc-by-name" [ LIBC_NAME "libc soname" need load-library ] }
        { "dlopen-libc-by-path" [ LIBC_PATH "libc path" need load-library ] }
        { "syscall-fn-openat" [ syscall-fn-openat ] }
        { "syscall-openat" [ syscall-openat ] }
        { "syscall-socket" [ syscall-socket ] }
        { "binding-read-etc-passwd" [ "read_file" "/etc/passwd" host-str ] }
        { "binding-read-granted-file" [ "read_file" "./stunt-fixture.txt" host-str ] }
        { "binding-getenv-home" [ "getenv" "HOME" host-str ] }
        { "callback-greet" [ callback-greet ] }
        { "callback-add" [ callback-add ] }
        { "callback-unregistered" [ callback-unregistered ] }
        { "map-get" [ map-get-row ] }
        { "map-set" [ map-set-row ] }
        { "map-overwrite-input" [ map-overwrite-input ] }
    } ;

:: check-line ( name expect -- line )
    name checks at :> fn
    fn [
        [ fn call( -- outcome detail ) ] [ "error" swap error>text ] recover :> ( outcome detail )
        expect "succeed" = outcome "succeeded" = and
        expect "refused" = outcome "refused" = and or
        expect "error" = outcome "error" = and or
        "PASS" "FAIL" ? :> verdict
        name outcome detail expect verdict "%s: %s (%s) - expected %s: %s" sprintf
    ] [ name expect "%s: not expressed in Factor - expected %s: FAIL" sprintf ] if ;
