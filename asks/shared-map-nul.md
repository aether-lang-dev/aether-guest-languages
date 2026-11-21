# The shared map keeps C strings: a key or a value with a NUL cannot be stored whole (ae 0.934.0)

Found 2026-11-21 fixing the NUL cut in `import quickjs`. The limit is not in
the QuickJS bridge's map glue: it is in Aether's runtime, `aether_shared_map.h`.

## Where the limit lives

```c
void aether_shared_map_put(AetherSharedMap* map, const char* key, const char* value);
const char* aether_shared_map_get(AetherSharedMap* map, const char* key);
int aether_shared_map_put_by_token(uint64_t token, const char* key, const char* value);
const char* aether_shared_map_get_by_token(uint64_t token, const char* key);
```

Keys and values are `const char*`: a NUL ends them on the way in (the stored
copy is `strdup`ed) and `get` returns a C string, so a value stored with a NUL
could not be read back whole either. The cross-process form
(`aether_shared_map_to_shm`) writes `key\0value\0key\0value\0\0`, which makes
the NUL a separator: a NUL inside a key or a value would corrupt the whole
serialization, not only that entry. The Aether-side calls the tests use
(`aether_shared_map_put(map, "k", "v")`) are these same functions, with `string`
parameters, so there is no length-aware way in from Aether either.

The QuickJS bridge's `aether_map_put(key, value)` and `aether_map_get(key)`
(`run_sandboxed_with_map`) used to hand these a script's string, which the
bridge had already cut at a NUL. They now refuse a key or a value with a NUL,
with an Error the script can catch ("aether_map_put: the value: refused: it
holds an embedded NUL; the shared map stores C strings ..."). Any other bridge that
exposes the map goes through the same C functions and so has the same limit
(not checked here).

## Repro

```aether
extern aether_shared_map_new(out_token: ptr) -> ptr
extern aether_shared_map_put(map: ptr, key: string, val: string)
extern aether_shared_map_get(map: ptr, key: string) -> string
// a map, then:
//   aether_shared_map_put(map, "k", "a\0b")
//   println(string.length(aether_shared_map_get(map, "k")))   // 1, not 3
```

## Proposed

Length-aware calls beside the C-string ones, and a length-prefixed shm
format (a frame per entry: lengths, then bytes) in place of the
double-NUL-terminated one, as there is no compatibility to keep:

```c
void aether_shared_map_put_n(AetherSharedMap*, const char* key, size_t key_len, const char* value, size_t value_len);
const char* aether_shared_map_get_n(AetherSharedMap*, const char* key, size_t key_len, size_t* value_len);
int aether_shared_map_put_by_token_n(uint64_t token, const char* key, size_t key_len, const char* value, size_t value_len);
const char* aether_shared_map_get_by_token_n(uint64_t token, const char* key, size_t key_len, size_t* value_len);
```

and, in `std`, `string`-typed Aether wrappers taking `@aether string`. When
they exist the bridges' map callbacks stop refusing.
