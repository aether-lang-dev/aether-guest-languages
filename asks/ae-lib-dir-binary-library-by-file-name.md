# `AETHER_LIB_DIR` does not find a binary library that `--lib` finds (ae 0.934.0)

Found 2026-11-21 while adding Rexx (rexx/). Not blocking: the tests pass the
directory with `--lib` instead (`LANG_LIB`, tests/run.sh and
tests/stunt_lib.sh). Written down for Aether's owners because the help text
says the two are the same thing: `AETHER_LIB_DIR` is "Same shape as --lib;
PATH-style list of module search dirs, searched after lib/ when no --lib is
given".

## What happens

An `--emit=lib` library is found by `import <name>` through `--lib <dir>`
when its file is `lib<name>.so` (`.dylib`), whatever source it was built
from. Through `AETHER_LIB_DIR=<dir>` the same file is found only when its
source's base name is `<name>`; otherwise the import is unresolved, and the
error lists `<dir>` among the places searched. RexxAE's library is built from
`src/engine.ae` and imported as `rexxae` (its README, "Link from Aether"), so
`AETHER_LIB_DIR` cannot be used for it.

## Repro (macOS arm64 and Linux, ae 0.934.0)

```sh
printf 'exports(twice)\ntwice(n: int) -> int { return n * 2 }\n' > tiny.ae
mkdir -p out && ae build --emit=lib tiny.ae -o out/libother.dylib   # .so on Linux
printf 'import other\nmain() { println("${other.twice(21)}") }\n' > use.ae

ae build use.ae --lib out -o a                      # Built: a; ./a prints 42
AETHER_LIB_DIR=$PWD/out ae build use.ae -o b        # error: unresolved import 'other' ... out ...
```

Named `out/libtiny.dylib` and imported as `tiny`, both ways build. So the
lookup under `AETHER_LIB_DIR` seems to go by the library's recorded source
name (`ae lib-info` shows `Source: tiny.ae`) where `--lib` goes by the file
name.

## What would help

The two lookups agreeing, by the file name as `--lib` does (that is the name
RexxAE documents). Until then this repository passes `--lib`.

## RESOLVED in Aether 0.935.0 (PR #3802), 2026-11-22

`AETHER_LIB_DIR` is now seeded before the binary-import prepass, so it finds
a binary library by its file name, as `--lib` does: the repro's `b` builds
and prints 42 on 0.935.0 (macOS arm64). The `LANG_LIB` hook is gone: Rexx's
lang.sh adds RexxAE's library directory to `AETHER_LIB_DIR`, and
tests/stunt_lib.sh gives the hosts their search path the same way (the two
are still alternatives: `AETHER_LIB_DIR` is searched only when no `--lib` is
given, so the hosts no longer pass `--lib`).
