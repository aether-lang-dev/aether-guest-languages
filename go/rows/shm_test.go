//go:build cgo

package rows

import (
	"bytes"
	"testing"
)

// A block as Aether's encoder writes it (runtime/aether_shared_map.c,
// aether_shared_map_encode): one frozen input "input" = "stunt-in" and one
// output with a NUL in its key and its value.
var aetherBlock = []byte{
	'A', 'E', 'S', 'M', 0, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0,
	5, 0, 0, 0, 8, 0, 0, 0, 'i', 'n', 'p', 'u', 't', 's', 't', 'u', 'n', 't', '-', 'i', 'n',
	3, 0, 0, 0, 3, 0, 0, 0, 'k', 0, 'y', 'v', 0, 'w',
}

func TestDecodeAetherBlock(t *testing.T) {
	m, err := shmDecode(aetherBlock)
	if err != nil {
		t.Fatalf("refused Aether's own block: %v", err)
	}
	if m.frozen != 1 || len(m.entries) != 2 {
		t.Fatalf("frozen %d, entries %d", m.frozen, len(m.entries))
	}
	if v, ok := m.get("input"); !ok || v != "stunt-in" {
		t.Errorf("input = %q", v)
	}
	if v, ok := m.get("k\x00y"); !ok || v != "v\x00w" {
		t.Errorf("the key and value with a NUL read %q, %v", v, ok)
	}
	if _, ok := m.get("k"); ok {
		t.Errorf("a key cut at its NUL was found")
	}
}

func TestEncodeIsAethersLayout(t *testing.T) {
	m := &shmMap{frozen: 1, entries: [][2]string{{"input", "stunt-in"}, {"k\x00y", "v\x00w"}}}
	if got := m.encode(); !bytes.Equal(got, aetherBlock) {
		t.Fatalf("encode differs from Aether's block:\n got %v\nwant %v", got, aetherBlock)
	}
}

func TestTrailingZerosAllowed(t *testing.T) {
	padded := append(append([]byte(nil), aetherBlock...), make([]byte, 4096-len(aetherBlock))...)
	if _, err := shmDecode(padded); err != nil {
		t.Fatalf("a block padded with zeros to a page (macOS) was refused: %v", err)
	}
}

// Every malformed block is refused whole, as Aether's reader refuses it.
func TestMalformedRefused(t *testing.T) {
	mod := func(f func(b []byte) []byte) []byte { return f(append([]byte(nil), aetherBlock...)) }
	cases := map[string][]byte{
		"layout 1 (frozen count, key\\0value\\0)": []byte("\x01\x00\x00\x00input\x00stunt-in\x00\x00"),
		"empty":                    {},
		"short header":             aetherBlock[:19],
		"no magic":                 mod(func(b []byte) []byte { b[0] = 'X'; return b }),
		"nonzero second word":      mod(func(b []byte) []byte { b[4] = 1; return b }),
		"version 3":                mod(func(b []byte) []byte { b[8] = 3; return b }),
		"257 entries":              mod(func(b []byte) []byte { b[16], b[17] = 1, 1; return b }),
		"an entry past the end":    mod(func(b []byte) []byte { b[20] = 200; return b }),
		"cut inside an entry":      aetherBlock[:len(aetherBlock)-2],
		"a byte after the last":    append(append([]byte(nil), aetherBlock...), 0, 0, 7),
		"frozen more than entries": mod(func(b []byte) []byte { b[12] = 3; return b }),
	}
	for name, b := range cases {
		if m, err := shmDecode(b); err == nil {
			t.Errorf("%s: read, %d entries, not refused", name, len(m.entries))
		}
	}
}
