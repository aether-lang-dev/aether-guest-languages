# Provenance

Unlike the other languages here, Rexx was not moved from aether's
`contrib/host`: aether never had a Rexx bridge. Everything in `rexx/` was
written here on 2026-11-21, and nothing of RexxAE is copied into this
repository.

| Here | From | Licence |
|---|---|---|
| the engine | [RexxAE](https://github.com/aether-lang-dev/RexxAE) at `REXXAE_REF` (`../ci/versions.env`), fetched and built by `../tests/rexx_env.sh`, never copied | MIT, copyright Paul Hammant (RexxAE's `LICENSE`) |
| the contained child's worker | RexxAE's `worker/worker.ae` at the same commit, built by `../tests/rexx_env.sh`, never copied | the same |
| `module.ae`, `test_host_rexx.ae`, `containment/checks.rexx`, `containment/surface.ae`, `containment/test_surface.sh`, `containment/lang.sh`, `../tests/rexx_env.sh` | new | MIT, this repository's (`../LICENSE`) |
| `containment/host.ae` | new, on the shape of `../quickjs/containment/host.ae` | the same |
| `containment/child_host.ae` | `../quickjs/containment/child_host.ae`, adapted: Rexx's function names, `GETENV` answered as a file is, the worker's own `ADDRESS` answer, `spin` cancelled rather than killed | the same |
| `containment/policy/module.ae` | `../quickjs/containment/policy/module.ae` (whose `add_grant` and `grants` are aether's `tests/integration/contrib_host_stunt/host.ae`), with the facts replaced by `script` and `ended_in_error` added | the same; aether is MIT |
| `containment/host_support.c` | `../quickjs/containment/host_support.c`, unchanged | the same |

`json_text` in `module.ae` and `rexx_json` in `containment/host.ae` follow
`json_string` in aether's `std/guest/module.ae` (MIT).

aether-child-sandboxing's `rexx/` showcase (`~/scm/aether-child-sandboxing`,
GitHub `aether-lang-dev/aether-child-sandboxing`) was read for its vocabulary
and its use of RexxAE's worker; no code was taken from it. That repository has
no `LICENSE` file as of 2026-11-21, which is worth fixing before anything is.

Keeping in step with RexxAE: move `REXXAE_REF` to RexxAE's `main` in a commit
of its own and run `GUEST_LANG=rexx sh tests/run.sh` (on Linux, with
`GHL_REQUIRE_CHILD=1`). If RexxAE adds a builtin, `test_surface.sh` sweeps it
at once (the names come from RexxAE's `tests/guest_functions.json`); if one
reaches anything, the sweep fails. If RexxAE changes its API (`include/rexxae.h`
and the `rexxae_*` exports), `module.ae` and the hosts fail to build.
