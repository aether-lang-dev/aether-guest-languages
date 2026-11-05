;; The rows of stunt's catalogue (aether contrib/host/stunt), each done the
;; way a Racket program would do it, and the judge that turns what happened
;; into stunt's one line:
;;
;;   <check>: <outcome> (<detail>) - expected <expectation>: PASS|FAIL
;;
;; The host (host.ae, or child_host.ae for the contained child) prepends a few
;; facts it found before the sandbox was in force (HOST_HOME_SET, HOST_SH,
;; LIBM_PATH, LIBC_NAME, LIBC_PATH), appends a call of (check-line name
;; expectation) and reads the line back. A refusal is what a sandbox gives:
;; EPERM or EACCES from libc, the preload library or the kernel, or a host
;; function's error that starts "refused:". Anything else that goes wrong is
;; an error, never a refusal, so it cannot pass for a sandbox at work.
;;
;; This file has no (require ...): the libraries it uses are required by the
;; host before the sandbox is in force (host.ae), or by the worker before it
;; locks itself down (racket/worker): Racket reads a module from disk the
;; first time it is required, and under a policy with no read grant on the
;; collects directory a module not yet required cannot be (racket/README.md).
;; They are ffi/unsafe, racket/tcp, racket/system and json (the worker's
;; protocol).

(define EPERM 1)
(define EACCES 13)   ; the same numbers on Linux, macOS, FreeBSD
(define FIXTURE "./stunt-fixture.txt")
(define WRITTEN "./stunt-written.txt")

(define (one-line s)
  (let ([t (regexp-replace* #rx"[\r\n]+" s " ")])
    (if (> (string-length t) 240) (substring t 0 240) t)))

;; The errno behind an exception: Racket's errno exceptions carry it, and the
;; message of one that does not still ends "errno=<n>".
(define (errno-of e)
  (cond [(exn:fail:filesystem:errno? e) (car (exn:fail:filesystem:errno-errno e))]
        [(exn:fail:network:errno? e) (car (exn:fail:network:errno-errno e))]
        [(exn? e) (let ([m (regexp-match #rx"errno=([0-9]+)" (exn-message e))])
                    (and m (string->number (cadr m))))]
        [else #f]))

(define (by-errno code what)
  (if (or (eqv? code EPERM) (eqv? code EACCES))
      (cons "refused" what)
      (cons "error" what)))

(define (exn-text e) (if (exn? e) (exn-message e) (format "~a" e)))

(define (first-line s)
  (let* ([l (car (regexp-match #rx"^[^\n]*" s))])
    (if (> (string-length l) 40) (substring l 0 40) l)))

;; open-input-file, then read: Racket's way to a file, through libc's open.
(define (open-path path)
  (with-handlers ([exn:fail? (lambda (e) (by-errno (errno-of e) (string-append "open: " (exn-text e))))])
    (let ([s (call-with-input-file path
               (lambda (in) (let ([t (read-string 4096 in)]) (if (eof-object? t) "" t))))])
      (cons "succeeded" (string-append "read \"" (first-line s) "\"")))))

(define (write-file)
  (with-handlers ([exn:fail? (lambda (e) (by-errno (errno-of e) (string-append "open: " (exn-text e))))])
    (call-with-output-file WRITTEN #:exists 'truncate
      (lambda (out) (display "written by racket\n" out)))
    (cons "succeeded" (string-append "wrote " WRITTEN))))

;; libc's getenv, through the FFI. (Racket's own getenv is libc's too: it
;; keeps no copy of the environment, so env-cached is left out.)
(define (libc-getenv)
  (with-handlers ([exn:fail? (lambda (e) (cons "error" (exn-text e)))])
    (let* ([f (get-ffi-obj "getenv" #f (_fun _string -> _bytes))]
           [v (f "HOME")])
      (cond [v (cons "succeeded" (string-append "HOME=" (bytes->string/utf-8 v #\?)))]
            [HOST_HOME_SET (cons "refused" "libc getenv gave NULL; HOME is in the environment")]
            [else (cons "error" "HOME is unset")]))))

;; tcp-connect, judged as stunt judges a connect: EACCES (the preload,
;; Landlock) or EPERM/EACCES at socket (seccomp) is a refusal; EPERM at
;; connect is a firewall's answer, an error; a connect still in progress or
;; refused by the network was let through. Nobody answers on TEST-NET-1, so a
;; connect that is let through is still waiting when the second is up.
(define (connect-to host)
  (let* ([result #f]
         [t (thread (lambda ()
                      (set! result
                            (with-handlers ([(lambda (e) #t) (lambda (e) e)])
                              (let-values ([(in out) (tcp-connect host 443)])
                                (close-input-port in)
                                (close-output-port out)
                                'connected)))))])
    (sync/timeout 2 t)
    (kill-thread t)
    (cond [(not result) (cons "succeeded" "let through: the connect was still in progress")]
          [(eq? result 'connected) (cons "succeeded" "connected")]
          [else
           (let ([code (errno-of result)])
             (cond [(eqv? code EACCES) (cons "refused" (string-append "connect: " (exn-text result)))]
                   [(eqv? code EPERM) (cons "error" (string-append "connect: " (exn-text result) " (a firewall's answer, not the sandbox's)"))]
                   [(not code) (cons "error" (string-append "connect: " (exn-text result)))]
                   [else (cons "succeeded" (string-append "let through: " (exn-text result)))]))])))

;; Starting /bin/sh with subprocess (rktio forks and execs; the child's
;; stderr is read, since a refused exec is reported there and as exit 1).
;; Succeeded when a shell started and exited 0; refused when subprocess
;; raised EPERM or EACCES, or the child's exec failed with one, or the shell
;; could not be executed (exit 127) while HOST_SH says /bin/sh is there.
(define (exec-sh)
  (with-handlers ([exn:fail? (lambda (e) (by-errno (errno-of e) (string-append "subprocess: " (exn-text e))))])
    (let-values ([(proc from-out to-in from-err) (subprocess #f #f #f "/bin/sh" "-c" "exit 0")])
      (close-output-port to-in)
      (let ([msg (port->string from-err)])
        (subprocess-wait proc)
        (let ([code (subprocess-status proc)]
              [m (regexp-match #rx"errno=([0-9]+)" msg)])
          (close-input-port from-out)
          (close-input-port from-err)
          (cond [(eqv? code 0) (cons "succeeded" "subprocess: exit 0")]
                [m (by-errno (string->number (cadr m)) (string-append "subprocess: exit " (number->string code) ": " (one-line msg)))]
                [(and (eqv? code 127) HOST_SH) (cons "refused" "subprocess: the shell could not be executed (exit 127)")]
                [else (cons "error" (format "subprocess: exit ~a: ~a" code (one-line msg)))]))))))

;; fork through the FFI, the child exiting at once: 0 succeeded, anything
;; else an error.
(define (fork-once)
  (with-handlers ([exn:fail? (lambda (e) (cons "error" (exn-text e)))])
    (let* ([c-fork (get-ffi-obj "fork" #f (_fun #:save-errno 'posix -> _int))]
           [c-exit (get-ffi-obj "_exit" #f (_fun _int -> _void))]
           [c-waitpid (get-ffi-obj "waitpid" #f (_fun _int _pointer _int -> _int))]
           [pid (c-fork)])
      (cond [(eqv? pid 0) (c-exit 0)]
            [(< pid 0) (by-errno (saved-errno) (format "fork: errno=~a" (saved-errno)))]
            [else
             (let* ([st (malloc 8 'raw)]
                    [r (begin (ptr-set! st _int 0 0) (c-waitpid pid st 0))]
                    [status (ptr-ref st _int)])
               (free st)
               (cond [(and (eqv? r pid) (zero? (bitwise-and status #x7f)) (zero? (arithmetic-shift status -8)))
                      (cons "succeeded" "the forked child ran")]
                     [else (cons "error" (format "the forked child: waitpid ~a, status ~a" r status))]))]))))

;; ffi-lib is a dlopen. A dlopen that fails on its own leaves a dlerror()
;; message, which Racket puts in the exception; the preload's refusal returns
;; NULL without one. That, or "Permission denied" or "Operation not
;; permitted" in the text, is the refusal.
(define (dlopen-row path)
  (with-handlers ([exn:fail? (lambda (e)
                               (let ([m (exn-text e)])
                                 ;; reading Racket's own files (system.rktd) is not the dlopen
                                 (if (and (not (regexp-match? #rx"open-input-file" m))
                                          (or (regexp-match? #rx"Permission denied|Operation not permitted" m)
                                              (regexp-match? #rx"system error: *(\\(null\\))?[ \r\n]*$" m)))
                                     (cons "refused" (string-append "ffi-lib: " m " (the preload's refusal)"))
                                     (cons "error" (string-append "ffi-lib: " m)))))])
    (ffi-lib path)
    (cons "succeeded" (string-append "loaded " path))))

;; Linux's system call numbers, for the two architectures stunt has raw
;; syscalls on.
(define (sys-number name)
  (let ([a (system-type 'arch)])
    (case a
      [(x86_64) (if (eq? name 'openat) 257 41)]
      [(aarch64) (if (eq? name 'openat) 56 198)]
      [else (error (format "no syscall numbers for ~a" a))])))
(define AT_FDCWD -100)

;; libc's syscall(), through the FFI: what stunt calls syscall-fn. Only EPERM
;; or EACCES is a refusal (ENOSYS is the preload's syscall() with no grants
;; in force, which is an error).
(define (syscall-fn-openat)
  (let* ([sc (get-ffi-obj "syscall" #f (_fun #:save-errno 'posix _long _long _bytes _long _long -> _long))]
         [r (sc (sys-number 'openat) AT_FDCWD #"/etc/passwd\0" 0 0)])
    (if (>= r 0)
        (cons "succeeded" "opened /etc/passwd")
        (by-errno (saved-errno) (format "syscall(): errno=~a" (saved-errno))))))

;; The syscall instruction itself, past libc and so past any preload: a few
;; bytes of machine code in an executable mapping, called through the FFI.
;; raw(nr, a, b, c, d) moves its arguments into the system call registers and
;; traps; the result is the kernel's, -errno on failure.
;;
;; The mapping is of a memfd, not anonymous memory: the preload refuses an
;; anonymous executable mmap and any mprotect to PROT_EXEC, but lets an
;; executable mapping of a file descriptor through (so the loader and JITs
;; can map code), and a memfd is one. So the program gets its system call
;; instruction, and only the kernel (Landlock, seccomp) is left to hold what
;; it does.
(define (raw-code)
  (case (system-type 'arch)
    ;; mov rax,rdi; mov rdi,rsi; mov rsi,rdx; mov rdx,rcx; mov r10,r8; syscall; ret
    [(x86_64) (bytes #x48 #x89 #xf8 #x48 #x89 #xf7 #x48 #x89 #xd6 #x48 #x89 #xca #x4d #x89 #xc2 #x0f #x05 #xc3)]
    ;; mov x8,x0; mov x0,x1; mov x1,x2; mov x2,x3; mov x3,x4; svc #0; ret
    [(aarch64) (bytes #xe8 #x03 #x00 #xaa #xe0 #x03 #x01 #xaa #xe1 #x03 #x02 #xaa #xe2 #x03 #x03 #xaa
                      #xe3 #x03 #x04 #xaa #x01 #x00 #x00 #xd4 #xc0 #x03 #x5f #xd6)]
    [else (error (format "no raw syscall for ~a" (system-type 'arch)))]))

(define raw-fn #f)
(define (raw-syscall)
  (or raw-fn
      (let* ([memfd-create (get-ffi-obj "memfd_create" #f (_fun _string _uint -> _int))]
             [ftruncate (get-ffi-obj "ftruncate" #f (_fun _int _long -> _int))]
             [mmap (get-ffi-obj "mmap" #f (_fun _pointer _size _int _int _int _long -> _pointer))]
             [fd (memfd-create "raw" 0)]
             [code (raw-code)])
        (ftruncate fd 4096)
        (let ([m (mmap #f 4096 7 1 fd 0)])   ; PROT_READ|WRITE|EXEC, MAP_SHARED
          (memcpy m code (bytes-length code))
          (set! raw-fn (function-ptr m (_fun _long _long _bytes _long _long -> _long)))
          raw-fn))))

(define (raw-result r what)
  (if (>= r 0)
      (cons "succeeded" what)
      (by-errno (- r) (format "the syscall instruction: errno=~a" (- r)))))

(define (syscall-openat)
  (let ([fn (raw-syscall)])
    (raw-result (fn (sys-number 'openat) AT_FDCWD #"/etc/passwd\0" 0 0) "the syscall instruction opened /etc/passwd")))

(define (syscall-socket)
  (let ([fn (raw-syscall)])
    ;; socket(AF_INET, SOCK_STREAM, 0): the arguments after the number are
    ;; longs, the third passed as the bytes type, so a null pointer is 0.
    (raw-result (fn (sys-number 'socket) 2 #f 1 0) "the syscall instruction made an AF_INET socket")))

;; A host function's answer. In-process (host.ae) a refusal is the exception
;; the host raised ("refused: ..."); in the contained child a file comes as
;; std.guest's answer to the call, a hash with granted/text or granted/why,
;; and a FAIL is an exception too.
(define (host-str name thunk arg)
  (with-handlers ([exn:fail? (lambda (e)
                               (let ([why (exn-message e)])
                                 (if (regexp-match? #rx"^refused:" why)
                                     (cons "refused" why)
                                     (cons "error" why))))])
    (let ([v (thunk)])
      (cond [(hash? v)
             (if (hash-ref v 'granted #f)
                 (cons "succeeded" (string-append name "(" arg ") = \"" (first-line (hash-ref v 'text "")) "\""))
                 (cons "refused" (string-append "refused: " (format "~a" (hash-ref v 'why "")))))]
            [(string? v) (cons "succeeded" (string-append name "(" arg ") = \"" (first-line v) "\""))]
            [else (cons "error" (string-append name " answered " (format "~a" v)))]))))

(define (callback-greet)
  (let ([v (host-greet "stunt")])
    (if (equal? v "hello, stunt")
        (cons "succeeded" "greet(\"stunt\") = \"hello, stunt\"")
        (cons "error" (format "greet(\"stunt\") = ~s" v)))))

(define (callback-add)
  (let ([v (host-add 2 40)])
    (if (equal? v 42)
        (cons "succeeded" "add(2, 40) = 42")
        (cons "error" (format "add(2, 40) = ~s" v)))))

;; A host function nobody registered is an undefined name in Racket: calling
;; it is an exception the program can catch, not a crash.
(define (callback-unregistered)
  (with-handlers ([exn:fail? (lambda (e)
                               (let ([m (exn-message e)])
                                 (if (and (regexp-match? #rx"nope" m) (regexp-match? #rx"undefined|unbound" m))
                                     (cons "refused" (string-append "no host function nope (" (exn-text e) ")"))
                                     (cons "error" m))))])
    (eval '(nope))
    (cons "succeeded" "nope() answered")))

;; The shared map, through the bridge's aether-map-get / aether-map-put (the
;; host's own map in the child).
(define (map-get)
  (let ([v (aether-map-get "input")])
    (cond [(equal? v "") (cons "error" "input is not in the map")]
          [(not (equal? v "stunt-in")) (cons "error" (format "input = ~s" v))]
          [else (cons "succeeded" "input = \"stunt-in\"")])))

(define (map-set)
  (aether-map-put "output" "from-racket")
  (let ([v (aether-map-get "output")])
    (if (equal? v "from-racket")
        (cons "succeeded" "output = \"from-racket\"")
        (cons "error" (format "output read back as ~s" v)))))

;; aether-map-put gives Racket no result, so a refused write is seen only by
;; reading the key back: still the host's value.
(define (map-overwrite-input)
  (aether-map-put "input" "overwritten")
  (let ([v (aether-map-get "input")])
    (if (equal? v "stunt-in")
        (cons "refused" "input is still \"stunt-in\" (read back)")
        (cons "succeeded" (format "input read back as ~s" v)))))

(define CHECKS (make-hash))
(define (check! name fn) (hash-set! CHECKS name fn))
(check! "open-etc-passwd" (lambda () (open-path "/etc/passwd")))
(check! "open-granted-file" (lambda () (open-path FIXTURE)))
(check! "write-granted-file" write-file)
(check! "getenv-home" libc-getenv)
(check! "connect-192.0.2.1" (lambda () (connect-to "192.0.2.1")))
(check! "connect-ipv4-mapped" (lambda () (connect-to "::ffff:192.0.2.1")))
(check! "exec-bin-sh" exec-sh)
(check! "fork" fork-once)
(check! "dlopen-libm" (lambda () (if LIBM_PATH (dlopen-row LIBM_PATH) (cons "error" "the host found no libm"))))
(check! "dlopen-libc-by-name" (lambda () (if LIBC_NAME (dlopen-row LIBC_NAME) (cons "error" "the host found no libc soname"))))
(check! "dlopen-libc-by-path" (lambda () (if LIBC_PATH (dlopen-row LIBC_PATH) (cons "error" "the host found no libc path"))))
(check! "syscall-fn-openat" syscall-fn-openat)
(check! "syscall-openat" syscall-openat)
(check! "syscall-socket" syscall-socket)
(check! "binding-read-etc-passwd" (lambda () (host-str "read_file" (lambda () (host-read-file "/etc/passwd")) "/etc/passwd")))
(check! "binding-read-granted-file" (lambda () (host-str "read_file" (lambda () (host-read-file FIXTURE)) FIXTURE)))
(check! "binding-getenv-home" (lambda () (host-str "getenv" (lambda () (host-getenv "HOME")) "HOME")))
(check! "callback-greet" callback-greet)
(check! "callback-add" callback-add)
(check! "callback-unregistered" callback-unregistered)
(check! "map-get" map-get)
(check! "map-set" map-set)
(check! "map-overwrite-input" map-overwrite-input)

(define (check-line name expect)
  (let ([fn (hash-ref CHECKS name #f)])
    (if (not fn)
        (string-append name ": not expressed in Racket - expected " expect ": FAIL")
        (let* ([r (with-handlers ([(lambda (e) #t) (lambda (e) (cons "error" (exn-text e)))])
                    (fn))]
               [outcome (car r)]
               [detail (one-line (cdr r))]
               [pass (or (and (equal? expect "succeed") (equal? outcome "succeeded"))
                         (and (equal? expect "refused") (equal? outcome "refused"))
                         (and (equal? expect "error") (equal? outcome "error")))])
          (string-append name ": " outcome " (" detail ") - expected " expect ": " (if pass "PASS" "FAIL"))))))
