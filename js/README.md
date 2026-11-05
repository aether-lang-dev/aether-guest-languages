# js: alias of duktape

`import js` is a back-compat alias for [`import duktape`](../duktape/README.md):
the embedded Duktape ES5.1 JavaScript engine. It exists so code written
against aether's `contrib.host.js` keeps working with a one-line change
(`import contrib.host.js` to `import js`); new code should `import duktape`,
because the engine name is the honest one and lets other JavaScript engines
sit beside it later under their own names.

```aether
import js

main() {
    js.run("print('hello from js: ' + Duktape.version);")
}
```

`module.ae` is a thin re-export: every `js_*` function calls the `duktape_*`
function of the same name, and the typed-value tags are `JS_TNIL` to
`JS_TOTHER` (`DUK_TNIL` to `DUK_TOTHER` in `duktape`). It imports `duktape`,
so the bridge's C is compiled in once; `js` and `duktape` are one bridge, not
two. The functions, the sandbox behaviour, the shared map, the loading of
libduktape and the containment table are all the
[duktape README](../duktape/README.md)'s.

| Aether call | Same as |
|---|---|
| `js.run(code)`, `js.run_sandboxed(perms, code)`, `js.run_sandboxed_with_map(perms, code, map_token)`, `js.init()`, `js.finalize()` | `duktape.run`, ... |
| `js.vm_*`, `js.arg_*`, `js.push_*`, `js.raise_error` | `duktape.vm_*`, ... |

[`test_js_alias.ae`](test_js_alias.ae) runs a script and a two-way VM with a
host callback through the alias (`../tests/run.sh` runs it with the rest).
