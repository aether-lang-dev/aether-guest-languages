# contrib.quickjs: an operation budget, and the exception as a handle (ae 0.934.0)

Found 2026-11-21 while giving `import quickjs` (quickjs/) the two-way VM the
Lua, Python, Duktape and Tcl bridges have. Not blocking: the bridge ships
without the first and works round the second, both marked in
`quickjs/aether_host_quickjs.c`. Written down for the next Aether batch.

## 1. An operation budget (for `vm_set_instruction_limit`)

Lua, Python and Tcl bridges stop a script after so many VM instructions
(`lua.vm_set_instruction_limit(vm, budget, every)`; Python counts trace
events, Tcl commands). QuickJS's bridge reports it unavailable, as Duktape's
does, because `contrib.quickjs` has a deadline and two cells but nothing that
counts: the interrupt handler (`interrupt_` in `aether_quickjs.c`) is the
engine's only poll, and it belongs to the module, so a bridge cannot add a
counter to it.

What is there to count: quickjs-ng calls the interrupt handler every
`JS_INTERRUPT_COUNTER_INIT` (10,000) steps of its interpreter, so a budget in
polls is a budget in units of about 10,000 operations, which is what the other
bridges' `every` already means.

Proposed API, beside the deadline:

```aether
// Stop JavaScript after `polls` more interrupt polls (each about 10,000
// interpreter steps), in this entry and later ones until set again; 0 for
// none. Stopped as for the deadline ("InternalError: interrupted").
set_poll_budget(q: ptr, polls: long)
poll_budget(q: ptr) -> long        // what is left
const INTERRUPTED_BUDGET = 4       // what interrupted() says
```

In C: a `int64_t budget` on `AeQjs`, decremented in `interrupt_` when it is
positive, `why = QJS_INTERRUPT_BUDGET` when it reaches zero, reported below a
cancel and a deadline. No process-wide state, as now.

Repro of the gap: there is no call to make.

```aether
import contrib.quickjs
main() {
    q = quickjs.runtime(0, 0)
    // nothing like quickjs.set_poll_budget(q, 100) exists: only a deadline,
    // which a slow machine and a fast one reach after different amounts of work
    quickjs.set_deadline_ns(q, quickjs.now_ns() + 100000000)
    _v, e = quickjs.eval(q, "for (;;) {}", "spin.js")
    println(e)            // InternalError: interrupted
    quickjs.dispose(q)
}
```

## 2. The pending exception as a handle

After a failed `eval`, `qjs_error(q)` is the exception's text with its stack
appended after a newline (`fail_`), and the value itself is kept for
`qjs_rethrow` (`q->exc`) but cannot be had as a handle. The bridge needs
`String(e)` alone for `vm_result_str` ("TypeError: x", as Python's
`Type: message`), so it cuts the text at the first `"\n    at "`. A message
that itself contains that text is cut short, and a stack format that changes
upstream breaks the cut.

Proposed API:

```aether
// The value the last failed eval, call, run_jobs or conversion threw, as a
// new handle to release; 0 when the failure had no value (a host-side
// refusal, a full heap).
last_exception(q: ptr) -> int
```

In C, `q->has_exc ? put_(q, JS_DupValue(q->ctx, q->exc)) : 0`. The bridge
would then read `String(e)` and `e.stack` separately, with `to_string` and
`get`.

Repro:

```aether
import contrib.quickjs
main() {
    q = quickjs.runtime(0, 0)
    _v, e = quickjs.eval(q, "throw new Error('a\\n    at b')", "x.js")
    println(e)   // "Error: a\n    at b\n    at <eval> (x.js:1:7)": which line is the stack?
    quickjs.dispose(q)
}
```

Update 2026-11-21: the bridge no longer cuts at `"\n    at "`: it reads an
error by catching what the engine's `rethrow` throws, in a `try` in a small
JavaScript function the VM keeps, and takes `String(e)` from that (which also
keeps a NUL in a message, see `contrib-quickjs-length-aware-strings.md`). A
`last_exception` handle would still replace that function and the second run of
a thrown object's own `toString` it costs.

## RESOLVED in Aether 0.935.0 (PR #3802), 2026-11-22

aether's `asks/REPLY-contrib-quickjs-op-budget-and-exception-handle.md`.

1. `vm_set_instruction_limit` is real: `qjs_set_poll_budget(q, ceil(budget /
   10000))` per outermost eval (cleared after it), reported as
   `InternalError: interrupted: instruction limit exceeded` (reason 4,
   `INTERRUPTED_BUDGET`). Its granularity is the engine's poll, one per
   10,000 counted steps: a budget of 100,000 stops `for (;;) G++` with `G`
   between 90,000 and 100,000 (`test_host_quickjs_vm.ae`, "the instruction
   limit: stops a loop, cannot be caught, per eval, nested, disarmed").
   QuickJS's "instruction limit: unavailable" cell is gone.
2. An error's message is `String(e)` of `qjs_last_exception`: `message_of`'s
   cut at `"\n    at "` and the catcher are deleted, so a message holding that
   text, a newline or a NUL is whole, and a thrown object's `toString` runs
   once.
