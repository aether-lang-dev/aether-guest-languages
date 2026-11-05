# rhombus: Rhombus on the embedded Racket CS VM

`import rhombus` runs [Rhombus](https://rhombus-lang.org), a language from the
Racket family with an indentation-sensitive infix surface (the *shrubbery*
notation) over Racket's macros and modules, in an Aether program. Rhombus is
a `#lang` on the Racket runtime, so there is no second VM: this module names
the bridge's `rhombus_*` entry points, which the Racket bridge
(`../racket/aether_host_racket.c`, compiled in because `rhombus/module.ae`
imports `racket`) already carries. It was `contrib.host.rhombus` in aether's
tree ([../racket/PROVENANCE.md](../racket/PROVENANCE.md)).

```aether
import rhombus

main() {
    fib = <<RHOMBUS
        fun fib(n):
          if n < 2 | n | fib(n-1) + fib(n-2)
        println(fib(10))
    RHOMBUS
    r = rhombus.evaluate(fib)                  // "55\n"
    rhombus.set("answer", "42")                // visible to racket.get
}
```

Everything in [../racket/README.md](../racket/README.md) applies: finding the
library, boot images and collects, the four ways (the contained child does not
work for Racket, so for Rhombus either), what run in-process under a sandbox
is and is not held, and that nothing stops a run that does not end.

## API

The same shape as `racket`, with `rhombus.` for `racket.`:

| Call | Effect |
|---|---|
| `rhombus.evaluate(code) -> string` | run `code` as the body of a `#lang rhombus` module and return what it printed (`println` adds a newline). On an error: what was printed, then `error: <message>` |
| `rhombus.run(code) -> int` | for effect; 0, or -1 on an error |
| `rhombus.set(key, value)` / `rhombus.get(key) -> string` | the k-v map, **the same one** as `racket.set`/`racket.get`: one VM, one hash |
| `rhombus.run_sandboxed(perms, code)`, `rhombus.run_sandboxed_with_map(perms, code, map_token)` | as racket's; the Racket glue `aether-map-get` / `aether-map-put` is how the map is reached |
| `rhombus.init()`, `rhombus.finalize()` | the shared VM's (`finalize` is a no-op) |

For the last error, host functions and the rest of the racket API use
`racket.` (`racket.last_error()`, `racket.register`): they are one VM's.

The code is wrapped as `#lang rhombus\n<code>`, read in memory with the
shrubbery reader, declared as a module under a fresh name for each call (so
a module cache never returns a stale instance) and required to run it. Each
call is therefore its own module: definitions do not carry to the next call
(use the k-v map, or `racket.run` for shared state).

## The `rhombus` collection

Racket's main distribution does not include it as of 9.3 (nor does Ubuntu
24.04's Racket 8.10 have a version that predates it): install it in the Racket
you load,

```sh
raco pkg install --auto rhombus
```

(user scope; the bridge finds it through the config directory's package
links). Its graph is large and is required at the first `rhombus.*` call, so
a program that only uses `racket.*` never pays for it.

## What was run

`test_host_rhombus.ae` (recursive `fib`, the `error:` contract, the shared
map in both directions with `fib0..fib9` written by Racket and read through
`rhombus.get`) passes on macOS 27.2 arm64 with Racket 9.3 and
`raco pkg install rhombus` (the version `download.rhombus-lang.org`'s catalog
gave on 2026-11-05), ae 0.889.0. It was not run anywhere else: Ubuntu 24.04's
Racket 8.10 has no `rhombus` collection, and `tests/run.sh` leaves the spec
out where `(collection-file-path "main.rkt" "rhombus")` fails, so it never
runs there.
