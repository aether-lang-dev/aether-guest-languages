# python: embedded CPython

`import python` lets an Aether program run Python. It is the bridge that
was aether's `contrib.host.python`, moved here ([PROVENANCE.md](PROVENANCE.md)):
the same functions under the same names, `python.run`,
`python.run_sandboxed`, `python.vm_new` and the rest. It loads the
machine's libpython at run time and needs no Python dev package to build.
CPython 3.10 to 3.14, the default (GIL) build.

## Four ways to run Python from Aether

From least to most contained. The first three run Python inside your
program, and the fourth runs it in a separate, locked-down process. All
four stay supported: pick by how far you trust the script, not by which is
newest.

| | 1. Embed | 2. Embed in a sandbox block | 3. Embed + `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| Where Python runs | your process | your process | your process | its own process (`python/worker`) |
| Your host functions | not checked | checked against the grants | checked against the grants | checked against the grants; each call is a message the host answers |
| Python's own file, socket, process and `ctypes` calls | not checked | **not checked** | checked against the grants (Linux), except `os.environ` | refused, beyond what the host hands over |
| Importing a module not imported yet | yes | yes | only with a read grant on the standard library | only what the worker imported before its lockdown |
| Platforms | everywhere libpython loads | same | Linux | Linux with Landlock; macOS refuses to start it |
| Needs | ae 0.875.0 | ae 0.875.0 | ae 0.875.0; `os.system` and `subprocess` held from 0.884.0 | ae 0.878.0 (`std.guest`) |

### 1. Embed: Python in your process, no containment

```aether
import python

main() {
    script = <<PY
        import json
        data = {"a": [1, 2, 3], 'b': "it's"}
        print(json.dumps(data, sort_keys=True))
        print(r"C:\no\escapes", "tab:\t|")
    PY
    if python.run(script) != 0 { println("python failed") }
    python.finalize()
}
```

A heredoc (`<<PY ... PY`, aether's language reference, "Heredoc strings")
carries the script verbatim: no `${}` interpolation and no escape
processing, so Python's quotes, raw strings and backslashes need no
escaping (`"tab:\t|"` reaches Python as written, and Python makes the tab),
and the indentation the lines share is stripped, which Python's own
indentation needs. The closing marker sits on a line of its own. A plain
string works too: `python.run("print('hi')")`.

`python.finalize()` at the end flushes what the script printed: the
embedded interpreter buffers its own stdout, and an Aether program that
exits without finalizing may lose it. The interpreter takes its stdout
encoding from the locale, as an embedded CPython does: under the C locale
(no `LANG`) a non-ASCII `print` raises `UnicodeEncodeError`.

For a script that calls back into the host, keeps state between runs or
returns a typed value, use a persistent VM: `python.vm_new`,
`vm_register` for host callbacks, `vm_eval`, `vm_result_*`
([Bidirectional embedding](#bidirectional-embedding-904) below). That is
still this first way: in-process and uncontained.

### 2. Embed inside a sandbox block

```aether
import python
import std.sandbox

main() {
    reader = sandbox.new("reader") { grant_env("HOME") }
    script = <<PY
        import os
        print(os.environ.get("HOME"))
    PY
    if python.run_sandboxed(reader, script) != 0 { println("python failed") }
    python.finalize()
}
```

`run_sandboxed` pushes the block's grants as a level for the length of the
call. Aether's own std calls and every host function the script reaches
are checked against them, on every platform. Python's own `open`, `socket`,
`os.system`, `subprocess` and `ctypes` go straight to libc, so on their own
they are **not** checked: this program prints `$HOME`, and the same script
could read `/etc/passwd`, connect anywhere or run a shell just as well.
Use this way for scripts you trust, where the grants describe what the host
lends them.

### 3. The same, under `LD_PRELOAD`

Run the program from way 2 with aether's sandbox preload, which sits
between CPython and libc and holds Python's own calls to the same grants
(Linux):

```sh
LD_PRELOAD="$(ae cflags --libs | tr ' ' '\n' | sed -n 's/^-L//p' | head -1)/libaether_sandbox.so" ./myprog
```

A script granted only `HOME`, run on CachyOS (CPython 3.14) under ae
0.885.0, first without the preload and then with it:

| Python does | without | with `LD_PRELOAD` |
|---|---|---|
| `os.environ["HOME"]` | `/home/paul` | `/home/paul` (granted) |
| `os.environ["USER"]` | `paul` | `paul`: **not held** |
| `libc.getenv(b"USER")` (`ctypes`) | `b'paul'` | `None` |
| `open("/etc/passwd")` | opens it | `PermissionError` (`EACCES`) |
| `os.system("true")` | `0`: runs `/bin/sh` | `-1`: refused |
| `socket` connect to 192.0.2.1:443 | goes out (nobody answers there) | `EACCES` |
| `import json` (not imported yet) | imports | `PermissionError` on `json/__init__.py` |

Two things in it are Python's own:

- **`os.environ` is not held.** CPython copies the environment into
  `os.environ` when it starts, before any sandbox, and `os.getenv` reads
  that copy: a dict lookup, which no libc interposer sees. Only libc's
  `getenv`, reached through `ctypes`, is held. This is stunt's `env-cached`
  row, and the bridge does not scrub the copy.
- **Imports need the standard library.** CPython reads a module from disk
  the first time it is imported, so under grants with no read of the
  standard library, `import json` fails unless json was imported before
  the call. Import what a script needs before `run_sandboxed`
  (`python.run("import json")`), or grant the directory:
  `grant_fs_read("/usr/lib/python3.14/*")`
  (`sysconfig.get_paths()["stdlib"]`).

It is a library interposer, not a kernel boundary: a raw system call made
without libc is not seen, and macOS has no equivalent. A script can make
one (`checks.py`'s `syscall-*` rows do, with a few bytes of machine code
mapped from a memfd, which the preload's refusal of anonymous executable
memory does not cover); only the contained child's kernel layer holds those.
`os.system` and `subprocess` are held from ae 0.884.0 (aether #3580);
before that they escaped it. The containment table below has every row.

### 4. A contained child, over `std.guest`

```aether
import std.guest
import std.sandbox
import std.list

main() {
    perms = sandbox.new("python") {
        grant_fs_read("/usr/lib/python3.14/*")      // CPython lists and reads its standard library to start
    }
    args = list.new()
    list.add(args, "libpython3.14.so.1.0")
    list.add(args, "json")                          // modules to import before the lockdown
    g, err = guest.launch(perms, "build/python-worker", args, "python", "")
    if err != "" { println("not started: ${err}") return }   // macOS: refused
    guest.mount(&g, "greet", 1, 1)
    script = <<PY
        import json
        print(greet("python"), json.dumps([1, 2]))
        open("/etc/passwd")
    PY
    guest.start(&g, script)
    while true {
        e = guest.next(&g)
        if e.kind == "SAY" {
            println(guest.printable(e.text))
        } else if e.kind == "CALL" {
            guest.answer(&g, guest.json_string("hello"))
        } else {
            println("${e.kind} ${guest.printable(e.text)}")
            break
        }
    }
}
```

On CachyOS this prints `hello [1, 2]`, then
`ERROR PermissionError: [Errno 13] Permission denied: '/etc/passwd'`.

Python runs in `python/worker`, a separate process `guest.launch` starts
under Landlock and seccomp from the policy, with the preload, and talking
to the host only over a socket (`std.ipc` frames). The worker loads
libpython, starts the interpreter and imports the modules the host names
(its second argument, comma-separated), then locks itself down with no
grants before the script starts. So the script reaches a file only when the
host hands it a descriptor, an environment variable only through a host
function, and a module only if it was imported before the lockdown. The
host can cancel a script at a deadline and kills a worker that stops
answering. On macOS, where nothing can contain the child, `guest.launch`
refuses and starts nothing. Build the worker once with
`ae build python/worker/worker.ae -o python-worker`; details under
[In a contained child](#in-a-contained-child) below.

## Stunt parity

Each of stunt's tests, as Python comes out of it in each way that runs the
checks: "matches" when every row of the test came out as stunt expects,
else the rows that did not and how.

<!-- stunt-parity:begin: generated by sh tests/run.sh --write-parity, from build/parity-*.tsv -->
| stunt test | policy | 2. Sandbox block, no preload | 3. Under `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| `test_stunt_exec` | `exec=/bin/sh,fork` | differs: open-etc-passwd not held | matches | from ae 0.884.0: differs: exec-bin-sh stricter, fork stricter; before: differs: fork stricter |
| `test_stunt_file_granted` | `fs_read=$WORK/stunt-fixture.txt,env=HOME` | differs: open-etc-passwd not held, write-granted-file not held | matches | differs: open-granted-file stricter, getenv-home stricter, env-cached stricter |
| `test_stunt_fork` | `fork` | differs: exec-bin-sh not held, open-etc-passwd not held | from ae 0.884.0: matches; before: differs: exec-bin-sh escapes | differs: fork stricter |
| `test_stunt_host_checked` | `-` | matches | matches | matches |
| `test_stunt_native` | `native=*` | differs: open-etc-passwd not held | matches | differs: dlopen-libc-by-name stricter, dlopen-libc-by-path stricter |
| `test_stunt_nested` | `fs_read=$WORK/*\|fs_read=/etc/passwd` then `fs_read=$WORK/*,fs_read=/etc/passwd\|fs_read=$WORK/*` | differs: open-etc-passwd not held, open-granted-file not held | matches | (no row of it runs this way) |
| `test_stunt_net_allowed` | `tcp=192.0.2.1:443` | differs: open-etc-passwd not held | matches | differs: connect-192.0.2.1 stricter, connect-ipv4-mapped stricter |
| `test_stunt_net_other` | `tcp=198.51.100.1:443` | differs: connect-192.0.2.1 not held, connect-ipv4-mapped not held | matches | matches |
| `test_stunt_no_grants` | `-` | differs: open-etc-passwd not held, open-granted-file not held, write-granted-file not held, getenv-home not held, env-cached not held, connect-192.0.2.1 not held, connect-ipv4-mapped not held, exec-bin-sh not held | from ae 0.884.0: differs: env-cached escapes; before: differs: env-cached escapes, exec-bin-sh escapes | matches |
| `test_stunt_write_granted` | `fs_write=$WORK/*` | differs: open-granted-file not held | matches | differs: write-granted-file stricter |

- Column 2, ae from 0.884.0: macOS 27.2 arm64, ae 0.885.0, Python 3.13.18
- Column 3, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.885.0, Python 3.14.9
- Column 3, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Python 3.14.9
- Column 4, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.885.0, Python 3.14.9
- Column 4, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Python 3.14.9
<!-- stunt-parity:end -->

The test names are stunt's, aether's `tests/integration/contrib_host_stunt`
at `AETHER_STUNT_REF` (`../ci/versions.env`), mirrored in
`../tests/test_stunt_*.sh` with the same policies (the contained child's
with a read grant on the standard library added, below); the containment
table below has each row. The table is generated from a run
(`sh tests/run.sh --write-parity`) and checked against every run
(`tests/parity.sh`), in the columns the host can run; the lines under it
say which host produced which column. FreeBSD's preload is not column 3's
(see the containment table).

## Containment table

What a Python script gets when it tries each row of stunt's catalogue
(aether `contrib/host/stunt`, at the ref in `../ci/versions.env`), done the
Python way ([containment/checks.py](containment/checks.py)), under the
policies of aether's `tests/integration/contrib_host_stunt`
(`../tests/test_stunt_*.sh`). "Per policy" means granted where the policy
grants it and refused where it does not, as stunt expects. The columns are
the ways above: "in-process, Linux (preload)" is way 3, "in-process, macOS"
is way 2 where there is no preload, and "contained child" is way 4. Every
cell is a test outcome (`../tests/run.sh`): checked on macOS arm64 (CPython
3.13), CachyOS x86_64 with Landlock (3.14; ae 0.885.0 and 0.878.0), Ubuntu
22.04 arm64 (3.10; preload and child) and GhostBSD amd64 (3.12, preload),
and in CI.

| Row | In-process, Linux (preload) | In-process, macOS | Contained child, Linux |
|---|---|---|---|
| `open-etc-passwd` (`open`) | refused | **not held**: reads it | refused |
| `open-granted-file` (`open`) | per policy | **not held** | refused under every policy: the file comes through `read_file` |
| `write-granted-file` (`open(.., "w")`) | per policy | **not held** | refused under every policy |
| `getenv-home` (libc's `getenv`, through `ctypes`) | per policy | **not held** | refused under every policy: the worker has no environment; `getenv` is a host function |
| `env-cached` (`os.environ`) | **escapes**: the copy CPython took at start | **not held** | refused under every policy: the worker is started with no environment |
| `connect-192.0.2.1`, `connect-ipv4-mapped` (`socket`) | per policy | **not held** | refused under every policy: the worker's own lockdown has no tcp grant |
| `exec-bin-sh` (`os.system` and `subprocess`) | per policy, from ae 0.884.0; before it **escapes**: both start `/bin/sh` with no exec grant | **not held** | refused under every policy, from ae 0.884.0: the worker's lockdown has no exec grant; before it, per policy (seccomp) |
| `fork` (`os.fork`) | (child-only row) | (child-only row) | refused under every policy: the worker's lockdown has no fork grant |
| `dlopen-libm` (`ctypes.CDLL`) | loads | loads | loads |
| `dlopen-libc-by-name`, `dlopen-libc-by-path` (`ctypes.CDLL`) | (child-only row) | (child-only row) | refused under every policy, `native=*` included |
| `syscall-fn-openat` (libc's `syscall()`, through `ctypes`) | (child-only row) | (child-only row) | refused |
| `syscall-openat`, `syscall-socket` (the syscall instruction, from machine code in a memfd mapping) | (child-only row) | (child-only row) | per policy: Landlock refuses the open, seccomp lets `socket(AF_INET)` through only with a tcp grant |
| `binding-read-etc-passwd`, `binding-read-granted-file`, `binding-getenv-home` (host functions) | per policy | per policy | per policy; a granted file goes down as a descriptor |
| `callback-greet`, `callback-add` | work | work | work |
| `callback-unregistered` | refused: a `NameError` the script can catch | same | same |
| `map-get`, `map-set` | work | work | work (the host's map) |
| `map-overwrite-input` | refused (read back: still the host's value) | same | same |
| `spin` (`while True: pass`) | stopped by the instruction limit | same | cancelled at the deadline, from the worker's trace hook |
| `silent` (`sys.settrace(None)`, the frame's `f_trace` too, then a loop) | (child-only row) | (child-only row) | killed: no answer to CANCEL within the grace period |
| `oversize` (one 1 MiB `print`) | (child-only row) | (child-only row) | killed: the frame is refused |
| `half-frame` (`os.write` of a frame's header and part of its payload to the worker's own socket, `AETHER_IPC_FD`) | (child-only row) | (child-only row) | killed: the host times out inside the frame |

None is left out: everything stunt does, Python's standard library can.

What the table says:

- **In-process is the easy default and is not containment on its own.**
  On Linux the preload holds Python's file, socket, `ctypes` and libc
  `getenv` calls, and from ae 0.884.0 `os.system` and `subprocess` too.
  Before 0.884.0 both escaped it: glibc's `system()` starts `/bin/sh` with
  `posix_spawn`, whose `clone` and `execve` happen inside libc, where the
  preload's `fork` and `execve` wrappers never saw them, and `subprocess`
  uses `posix_spawn` too on CPython 3.13 and later. `os.environ` is never
  held in-process. On macOS nothing holds Python's own calls at all. The
  host functions (`binding-*`), which check the grants themselves, hold
  everywhere.
- **The contained child holds every row it runs**, on Linux, and is
  stricter than the policy: the worker locks itself down with no grants
  before the script starts, so a script reaches a granted file only through
  the host and never gets a handle on libc, a socket, a fork or (from ae
  0.884.0) a shell. The kernel layer from the policy is what holds the
  rows that go past libc (`syscall-openat`, `syscall-socket`); they follow
  the policy. Before 0.884.0 process start followed the policy too
  (seccomp lets `execve` through only with an exec grant).
- **The contained child's policy has one grant more than stunt's**: a read
  of the standard library's directories (`sysconfig.get_paths()["stdlib"]`
  and `["platstdlib"]`), which `../tests/stunt_lib.sh` passes as
  `GHL_PYTHON_STDLIB` and `containment/child_host.ae` adds to every policy.
  CPython cannot start without it: `spawn_socket`'s Landlock floor lets the
  loader read libraries but lists no directory, and CPython lists its
  standard library to import `encodings` ("Fatal Python error: Failed to
  import encodings module"). Any host of `python-worker` has to give it. No
  row reads there.
- **On FreeBSD** (GhostBSD 15, CPython 3.12, ae 0.885.0) the in-process
  rows come out as on Linux but for `exec-bin-sh`: FreeBSD's
  `libaether_sandbox.so` answers `fork()` and `system()` with `ENOSYS`
  even when fork and exec are granted (`posix_spawn` and `vfork` get
  through), so `os.system` and `subprocess` cannot start a shell under any
  policy. That is a bug in the preload, not a refusal; the harness runs it
  as `PRELOAD FAILS`, expecting the error. FreeBSD's runs are not in the
  parity table's column 3, which is Linux's.
- The tests hold these cells, including the gaps: an **escapes** or **not
  held** row runs expecting the call to succeed, so the day the preload
  holds it that test fails and this table is brought up to date. The
  `exec-bin-sh` row is checked on either side of ae 0.884.0
  (`AE_SPAWN_PIN` in `ci/versions.env`), from the ae the tests run on.

### Importing under a sandbox

Not a row of stunt's, but what decides whether a script runs at all:
CPython reads a module from disk the first time it is imported.
[containment/test_imports.sh](containment/test_imports.sh) holds it:

- in-process under the preload with no grants, `import json` (not imported
  yet) is refused, `PermissionError` on the open of its source; without the
  preload (macOS) it imports;
- in-process with a read grant on the standard library, it imports;
- in the contained child, under any policy, `import decimal` (which the
  worker did not import before its lockdown) is refused, and
  `import socket` (which it did) works.

## Loading libpython

The bridge's C (`aether_host_python.c`) is compiled into the program that
imports `python` (`@source` in `module.ae`); there is no archive to build
first and no `aether.toml` entry to add. libpython is not linked: the
bridge `dlopen`s it at the first call (with `RTLD_GLOBAL`, so the extension
modules CPython loads later, `ctypes` among them, resolve their `Py_*`
symbols against the same library), and a program built on one machine runs
against whatever CPython 3.10 to 3.14 the machine it runs on has.

Where the bridge looks for libpython, in order:

1. `${AETHER_PYTHON_SONAME}`: an exact soname or path
   (`libpython3.12.so.1.0` on Ubuntu 24.04, `/usr/lib/libpython3.14.so.1.0`
   on Arch, `/usr/local/lib/libpython3.12.so.1.0` on FreeBSD,
   `/opt/homebrew/opt/python@3.13/Frameworks/Python.framework/Versions/3.13/Python`
   with Homebrew on macOS);
2. `libpython3.so`, the stable-ABI library, where the system has one
   (Arch and Fedora do; Debian and Ubuntu ship it only in the dev package).

There is no list of versions to try: the program that starts yours owns
that choice. `../tests/python_env.sh` is one way to make it, from a
`python3`'s `sysconfig` (on Linux and FreeBSD this line is enough; a macOS
framework build names its library relative to `PYTHONFRAMEWORKPREFIX`,
which `python_env.sh` handles):

```sh
AETHER_PYTHON_SONAME=$(python3 -c 'import sysconfig as s, os; n = s.get_config_var("INSTSONAME"); print(os.path.join(s.get_config_var("LIBDIR"), n))')
```

A free-threaded CPython (3.13t, 3.14t) is refused at load with a message
(`python_abi.h` says why).

## In a contained child

`python/worker/worker.ae` is a std.guest worker. It takes the libpython
soname as its first argument, since it is given no environment, and a
comma-separated list of modules to import as its second; it loads
libpython, starts the interpreter and imports them before it locks itself
down. In the script, `print` is `SAY`, and each function the host mounts is
a global of the same name whose call is a `CALL` the host answers: the
arguments go as JSON, the answer comes back decoded (a granted file is
`{"granted": True, "text": ...}`), a `FAIL` raises `RuntimeError(message)`
and a `CANCEL` raises `RuntimeError("cancelled by host: ...")`. A
`sys.settrace` hook looks for a `CANCEL` every 1000 trace events. A script
that removes it, or catches the `RuntimeError` (CPython then drops a
Python-level trace function itself), stops answering, and the host kills
it after its grace period: the containment is the host's, not the hook's.
[containment/child_host.ae](containment/child_host.ae) is a complete host.

Whatever the policy, the worker's process can read the standard library's
directories (it must, to start), and its environment holds std.ipc's
`AETHER_IPC_FD` and `AETHER_IPC_KIND` and the `AETHER_PYTHON_SONAME` it set
itself, which is how a script finds its own socket (the `half-frame` row).

## Bidirectional embedding (#904)

`run_sandboxed` runs a script and reports success. A host that uses Python
as an extension language needs more: a **persistent VM** the host owns,
host functions the script can call, globals the host injects, and **typed**
values both ways.

```aether
import python

@c_callback
app_add(vm: ptr) -> int {
    a = python.arg_int(vm, 1)
    b = python.arg_int(vm, 2)
    python.push_int(vm, a + b)
    return 1
}

main() {
    vm = python.vm_new()
    python.vm_register(vm, "app.math.add", app_add)
    python.vm_set_global_int(vm, "BASE", 100)

    python.vm_eval(vm, "app.math.add(2, 3) + BASE")
    if python.vm_result_type(vm) == python.PY_TINT {
        println("${python.vm_result_int(vm)}")   // 105
    }
    python.vm_pop_result(vm)
    python.vm_free(vm)
}
```

| Call | Effect |
|------|--------|
| `python.vm_new() -> ptr` | a persistent VM: its own globals dict, with `__builtins__` |
| `python.vm_register(vm, name, fn) -> int` | registers an Aether `@c_callback (vm: ptr) -> int` as a Python function; `"app.math.add"` makes (or reuses) the modules `app` and `app.math` |
| `python.vm_set_global_str/_int/_strlist(vm, name, v)` | injects a global (str, int, or a list of str) |
| `python.vm_eval(vm, code) -> int` | compiles `code` as an expression, or failing that as statements, and runs it once: an expression gives its value, statements give `None`. 0, or -1 on an exception |
| `python.vm_result_type(vm) -> int` | the result's tag (`PY_T*`) |
| `python.vm_result_int/_double/_str/_bool(vm)` | read the result (see below) |
| `python.vm_result_bytes(vm) -> ptr` | a `bytes` result as a new `std.bytes` handle (the caller frees it), or null |
| `python.vm_pop_result(vm)` | releases the result |
| `python.vm_set_instruction_limit(vm, budget, every) -> string` | bounds each outermost eval to `budget` trace events; 0 disarms; `""` when set |
| `python.arg_count/_type/_int/_double/_str/_bool/_bytes(vm, i)` | **inside a callback**: the arguments, 1-based, read like results |
| `python.push_int/_double/_str/_bool/_nil/_bytes(vm, v)` | **inside a callback**: the return value (the last push wins; return 1 to return it, 0 for `None`) |
| `python.raise_error(vm, msg) -> int` | **inside a callback**: `return python.raise_error(vm, msg)`; the script sees `RuntimeError(msg)` after the callback returns, so its defers run |

The tags are `PY_TNIL`, `PY_TBOOL`, `PY_TINT`, `PY_TSTR`, `PY_TTABLE` (a
list or dict), `PY_TERR`, `PY_TOTHER`, `PY_TBYTES` and `PY_TFLOAT`; the
first seven are Lua's numbering. How values read:

- **`bool` is not `int`.** Python's `bool` subclasses `int`, so `True` is
  checked first and tags `PY_TBOOL`. `_bool` reads 1 only for `True`; it
  does not take an int's truth.
- **Numbers read either way.** `_double` of an int is that number; `_int`
  of a float truncates, saturating at the int64 range.
- **Integers past 64 bits** (`2**64`) tag `PY_TOTHER`. `_int` reads 0,
  never a wrapped value; `_double` reads the nearest double (infinity past
  ~1e308).
- **Text and bytes stay apart.** A `str` reads with `_str` (UTF-8; it stops
  at an embedded NUL) and never as bytes; a `bytes` reads with `_bytes`,
  length and zeros intact, and never as text. `_str` is "" for any other
  type.
- **Errors.** A failed eval tags `PY_TERR`; `_str` is
  `"<ExceptionType>: <message>"`. The error indicator is always cleared.
- Tags are the value's real type: an object whose `__class__` claims to be
  `bytes` is still `PY_TOTHER`.

### One interpreter, many VMs

CPython has one interpreter per process, and a VM is a globals dict on it.
**Two VMs are not isolated**: they share `sys.modules`, everything
imported, `builtins`, and the GIL. A global set in one VM is not visible in
another, but anything reachable through a module is. A host function
stored somewhere shared can outlive its VM; calling it then raises
`RuntimeError` instead of reaching the freed VM.

Every call takes the GIL (`PyGILState_Ensure`), so any thread (an actor's
scheduler thread included) may use a VM, one thread at a time per VM.
Callbacks run with the GIL held and may call back into the bridge,
including `vm_eval` on their own VM: a nested eval keeps the outer call's
arguments and pushed value apart from its own.

`python.finalize()` ends the interpreter, and every VM with it. Call it
from the thread that called `init`. When the process already had an
interpreter (Aether loaded into Python), the bridge uses it and never
finalizes it.

### The instruction limit

The limit is a `PyEval_SetTrace` hook installed on the calling thread for
the outermost `vm_eval`; a callback's nested eval neither resets the budget
nor removes the hook. It counts trace events (lines, calls, returns);
`every` is accepted for the other bridges' signature and ignored. Once the
budget is spent the hook raises `RuntimeError("instruction limit
exceeded")` on **every** later event, so a script that catches it (`except
BaseException: pass`) is stopped again at its handler's next line.

It bounds Python code only. It does not bound:

- a loop inside C, such as `sum(range(10**12))`, which makes no trace
  events;
- a script that calls `sys.settrace` and so replaces the hook;
- threads the script starts.

The contained child (way 4) is the answer to those: its host cancels at a
deadline and kills a worker that does not answer.

## Implementation notes

- All CPython C API access goes through a `dlsym` function-pointer table:
  see `aether_host_python.c`. The bridge has no unresolved `Py_*` symbols,
  and needs none of CPython's headers: the types it uses are in
  `python_abi.h`, which says why each is the same from 3.10 to 3.14.
- Reference counts go through `Py_IncRef` and `Py_DecRef`, never the
  `Py_INCREF` macros, which read the object header inline;
  `Py_RETURN_NONE` is `Py_IncRef(None)` and `return None`.
- The `_aether_map` module behind `aether_map_get` and `aether_map_put` is
  made with `PyModule_New` and a `PyCFunction_NewEx` per function, not from
  a `PyModuleDef`, whose embedded object header differs between Python
  builds.
- A free-threaded libpython is refused at load: it exports
  `_Py_DecRefShared`, which the default build does not.
- `aether_map_put` returns `None` whether or not it wrote, so a script
  cannot see that a write to a frozen input was refused (the containment
  table's `map-overwrite-input` row reads the key back).

## Tests

- [`test_host_python.ae`](test_host_python.ae): the fire-and-forget
  surface and the basics of the VM tier, through `import python`: a script
  evaluates, the interpreter really runs (state persists between runs),
  runtime and syntax errors are failures, `init` is idempotent, host
  functions with typed values and bytes, errors, nested evals and the
  limit. Moved from aether unchanged but for the import and the
  `describe` name.
- [`test_host_python_vm.ae`](test_host_python_vm.ae): aether's
  `host_python_bidirectional` and `host_python_map_context` C harnesses as
  an Aether spec with `@c_callback` host functions: dotted registration,
  every argument and result type, bytes with embedded zeros, 64-bit
  overflow, a lying `__class__`, exceptions, `raise_error`, nested evals,
  two VMs, a host function outliving its VM, the instruction limit (caught,
  nested, per eval), two threads, flat refcounts, gc objects and memory, a
  nested `run_sandboxed_with_map`, and finalize then init again.
- `../tests/test_stunt_*.sh`, `../tests/test_catalogue.sh` and
  `containment/test_imports.sh`: the containment table and the parity
  table above.

Both specs skip when libpython cannot be loaded (on GitHub Actions a skip
is a failure). They assert on return codes and Python-side state, never on
the interpreter's stdout. `../tests/run.sh` runs everything.
