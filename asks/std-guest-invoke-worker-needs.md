# What the workers here needed of `std.guest`'s host-calls-guest verb

From the guest side of the verb (`READY`, `INVOKE`, `RETURN`, `RAISE`, `END`),
built on branch `guest-invoke` against aether's `explore/guest-host-calls-guest`
(the prototype's wire format and API): eight workers (Python, Lua, QuickJS,
Duktape, Tcl, Perl, Ruby, and Rexx on RexxAE) serve exports with
`std.guest.child`'s `ready`, `serve`, `returned` and `raised`, and every one of
them could be written on what is there. Nothing here is a blocker. These are the
places where the API made every worker repeat itself or where a limit is
enforced later than it could be, each with the change proposed.

## 1. An oversized `RETURN` kills a worker that could have raised

A `RETURN` past `MAX_FRAME` goes out and the host refuses the frame and kills
the worker (`guest-export-oversize`: held, by the kill). For a guest that only
returned too much, a `RAISE` would keep it serving, and the worker is the one
that knows the size before sending.

Proposed: `child.returned(h, json)` returns `""`, or refuses a value longer
than `MAX_FRAME - 7` without sending it and says why; the worker then sends
`child.raised(h, "the value is N bytes, over the frame limit")`. The host's
refusal and kill stay for a worker that sends one anyway (the row still holds).
`call_host` could do the same for a `CALL`'s arguments.

## 2. An `INVOKE` argument past the worker's frame limit

The host's `g.max_frame` bounds what it reads, not what `invoke` writes; the
worker reads at most `MAX_FRAME`, so a 100,000-byte argument breaks its read
and it exits (each language's grid, `big-arg`). The host could refuse before
sending, as it refuses a name that is not exported.

Proposed: `guest.invoke` refuses (`""` or why, nothing sent, the guest still
serves) when `INVOKE name args` is longer than `MAX_FRAME`.

## 3. Every worker writes the same serving loop

Each worker has the same twenty lines after its script: `ready`, then `serve`
until `END` (`done(h, 0)`) or `CANCEL` (`error(h, "cancelled by host: ...")`),
running the engine for each `INVOKE`, then checking `h.cancel` (a cancel heard
inside the invocation ends the worker) before `returned` or `raised`. And the
loop cannot be named `serve` (below).

Proposed: `child.serve_exports(h, names) callback { ... }`, the block given
`inv.name` and `inv.args` and answering with a `(kind, text)` the module sends,
the `END`, `CANCEL` and post-invocation cancel handled once. Optional: the
workers are fine as they are.

## 4. The compiler trap the design note recorded is still there

Inside `child.run(h) callback { ... }`, an unqualified call of a local function
named like one of `child`'s exports resolves to the export (the design note's
open question 7, released 0.934.0). Every worker's loop is named
`serve_exports` for it. A fix in the compiler, not in `std.guest`.

## 5. Nothing the workers needed that is missing

`Host.invoked` (the arguments, fetched from an engine callback rather than
spliced into source) is what made a 60 KB argument cost a copy in every
worker; `is_mounted` let each refuse a mount under its export name. Keep both.

## RESOLVED on aether's `batch/guest-invoke` (9ab5efde9), 2026-11-22

1. `child.returned` returns why it sent nothing for a value too large for
   the host's frame; `child.serve_exports` raises it and the worker serves on
   (each language's grid: `big`, now "refused (`RAISE`), the guest serves on").
2. `guest.invoke` refuses an `INVOKE` frame over 64 KiB before sending, and
   the guest serves on (`big-arg`). A host that lowers `max_frame` tells the
   worker with `MAXFRAME` before the script (`maxframe`, a new case).
3. `child.serve_exports(h) callback | sh, inv | { ... }` is the serving loop;
   every worker here uses it, and each worker's own loop is gone. The
   workers' helper that calls it is named `serve_guest`: the callback-block
   name resolution (item 4, still open in the compiler) would have bound an
   unqualified `serve_exports` to the new export.
