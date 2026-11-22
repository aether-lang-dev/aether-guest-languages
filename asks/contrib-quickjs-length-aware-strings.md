# contrib.quickjs: take strings in by length, and keep an error's text whole (ae 0.934.0)

Found 2026-11-21 fixing the NUL cut in `import quickjs` (quickjs/): a string
with an embedded NUL was silently cut at the NUL when it crossed the bridge.
The bridge now carries the NUL everywhere the engine allows and refuses it,
loudly, where it does not (quickjs/README.md, "Strings, NUL and surrogates").
That needed workarounds that a few length-aware calls in `contrib.quickjs`
would make unnecessary. Not blocking; written down for the next Aether batch.

## What is length-aware and what is not

Reading out is: `to_string` returns the byte count and `qjs_scratch` holds
the bytes, NULs kept (the module's comment says so). Everything that takes a
string in is a C string, and so is the recorded error:

| call | what a NUL does |
|---|---|
| `new_string(q, s)` (`qjs_string`, `JS_NewString`) | the string ends at the NUL |
| `eval(q, code, filename)` (`qjs_eval`, `strlen(code)`) | the script ends at the NUL; the part before it runs, with no error |
| `throw_error(q, kind, msg)` (`qjs_throw`, `"%s"`) | the message ends at the NUL |
| `parse_json(q, text)` (`strlen(text)`) | `1\0garbage` parses as `1` |
| `get`, `set` names | a different, shorter name |
| a failed call's text (`fail_`, `JS_ToCString` then `strdup`) | `last_error`, and the error half of `eval`'s tuple, end at the NUL: `throw new Error('x\0y')` reads back as `Error: x` |

Repro (all three in one program, no bridge):

```aether
import contrib.quickjs
import std.string

main() {
    q = quickjs.runtime(0, 0)
    h = quickjs.new_string(q, "a\0b")
    s, _e = quickjs.to_string(q, h)
    println(string.length(s))              // 1, not 3
    quickjs.release(q, h)

    _v, err = quickjs.eval(q, "throw new Error('x\\0y')", "x.js")
    println(string.substring(err, 0, string.index_of(err, "\n")))   // "Error: x", not "Error: x\0y"

    v2, _e2 = quickjs.eval(q, "var RAN = 1;\0 var NOT_RAN = 2;", "y.js")
    println(v2 > 0)                        // true: it ran, up to the NUL, and said nothing
    quickjs.dispose(q)
}
```

## Proposed

Make the string parameters `@aether string` and use the length, as the
bridges' externs now do (`aether_string_data`, `aether_string_length`):

```aether
new_string(q: ptr, s: @aether string) -> int        // JS_NewStringLen(ctx, data, length)
eval(q: ptr, code: @aether string, filename: string) -> (int, string)   // JS_Eval(ctx, data, length, ...)
throw_error(q: ptr, kind: string, msg: @aether string) -> int   // JS_NewStringLen for the message
parse_json(q: ptr, text: @aether string) -> (int, string)       // JS_ParseJSON(ctx, data, length, ...): a NUL is then a parse error
last_error(q: ptr) -> string @heap                   // string_new_with_length(err, err_len)
```

In C: `fail_` uses `JS_ToCStringLen` and keeps the length beside `q->err`
(`set_err_` as well); `qjs_string`, `qjs_eval`, `qjs_throw` and `qjs_parse_json`
take a length. A script text with a NUL then just runs (JavaScript allows a raw
NUL inside a string literal), and the bridge's refusal of script text with a
NUL, its piecewise string construction (a JavaScript function joining the
pieces with a NUL), its catcher for the exception text and its thrower for a
message with a NUL can all go.

Until then the bridge carries what it can through JavaScript helpers the VM
keeps and refuses script text and JSON text with a NUL.

## RESOLVED in Aether 0.935.0 (PR #3802), 2026-11-22

aether's `asks/REPLY-contrib-quickjs-length-aware-strings.md`. The module
takes every string in by its length (`@aether string`) and hands every string
out with it (`qjs_error_data` and `qjs_error_len` replace `qjs_error`;
`qjs_to_json` returns a byte count), and hands out valid UTF-8 itself. The
bridge (`quickjs/aether_host_quickjs.c`) is on that ABI: the joiner and the
piecewise `new_string`, the thrower, the catcher and the lone-surrogate scrub
are deleted, and so are the refusals of a NUL in script text, a name and JSON
text (`WHY_CODE`, `WHY_NAME`, `WHY_JSON`): each now carries the NUL (a raw NUL
in JSON text is a `SyntaxError`, as any stray character). A filename with a
NUL is still the engine's refusal; the bridge never passes one on.
`test_host_quickjs_vm.ae`'s "what cannot carry a NUL is refused" became
"script text, names and JSON text carry a NUL too".
