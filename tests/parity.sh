#!/bin/sh
# The "Stunt parity" table in <lang>/README.md, made from a run.
#
#   tests/parity.sh check <lang>    fail when the README's table disagrees
#                                   with what the last run recorded
#   tests/parity.sh write <lang>    rewrite the README's table from it
#
# tests/stunt_lib.sh appends every outcome to build/parity-<lang>.tsv (test,
# column, ae side, row, kind), and tests/run.sh clears it before a language's
# containment run, then calls this. One table row per stunt test
# (tests/test_stunt_*.sh, named after aether's contrib_host_stunt tests at
# AETHER_STUNT_REF), one column per way the checks run in:
#
#   2  the sandbox block with no preload (macOS)
#   3  the same under LD_PRELOAD (Linux)
#   4  the contained child (Linux with Landlock)
#
# A cell is "matches" when every row of the test came out as stunt expects,
# else "differs:" and each row that did not, with how (escapes, not held,
# stricter, left out). Only the columns this host ran are checked or written;
# the others are kept as they are, so the table is put together from the
# hosts that can run each column, and a line under it says which host
# produced which.
#
# The exec rows differ either side of AE_SPAWN_PIN (ci/versions.env), so a
# cell holds both sides when they differ: "from ae X: ...; before: ...". A
# run reads and writes its own side; a cell with one value stands for both
# until a run on the other side says otherwise.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
. "$ROOT/ci/versions.env"

what="${1:-}"
lang="${2:-}"
case "$what" in check|write) ;; *) echo "usage: parity.sh check|write <lang>"; exit 2 ;; esac
[ -n "$lang" ] || { echo "usage: parity.sh check|write <lang>"; exit 2; }
README="$ROOT/$lang/README.md"
RESULTS="$ROOT/build/parity-$lang.tsv"
PROV="$ROOT/build/parity-$lang.host"
if [ ! -s "$RESULTS" ]; then
    echo "  [SKIP] parity ($lang): nothing was recorded"
    exit 0
fi

# The tests in order, each with its policy lines.
TESTS="$ROOT/build/parity-tests.tsv"
: > "$TESTS"
for t in "$SCRIPT_DIR"/test_stunt_*.sh; do
    name="$(basename "$t" .sh)"
    pol="$(sed -n 's/^policy "\(.*\)"$/\1/p' "$t" | awk 'NR > 1 { printf " then " } { gsub(/\|/, "\\|"); printf "`%s`", $0 }')"
    printf '%s\t%s\n' "$name" "$pol" >> "$TESTS"
done

out="$ROOT/build/parity-$lang.md"
awk -v what="$what" -v pin="$AE_SPAWN_PIN" -v provfile="$PROV" -v out="$out" -F '\t' '
function kindtext(k) { k = tolower(k); return k }
function sidevalue(cell, side,    i, f, b) {
    # "from ae X: A; before: B" or a single value for both.
    i = index(cell, "; before: ")
    if (substr(cell, 1, 8) != "from ae " || i == 0) return cell
    f = substr(cell, 1, i - 1); sub(/^from ae [^:]*: /, "", f)
    b = substr(cell, i + 10)
    return side == "from" ? f : b
}
function compose(f, b) {
    if (f == b) return f
    if (f == "") return b
    if (b == "") return f
    return "from ae " pin ": " f "; before: " b
}
function trim(s) { gsub(/^ +| +$/, "", s); return s }
FILENAME == ARGV[1] {   # the tests
    ntests++; tname[ntests] = $1; tpol[$1] = $2; next
}
FILENAME == ARGV[2] {   # the results
    t = "test_" $1; c = $2; side = $3
    if (c !~ /^[234]$/) { other[c] = 1; next }   # a column the table has not (3f)
    ran[c] = side; seen[t, c] = 1
    if ($5 != "PASS") {
        item = $4 " " kindtext($5)
        if (index("\n" diff[t, c] "\n", "\n" item "\n") == 0)
            diff[t, c] = diff[t, c] (diff[t, c] == "" ? "" : "\n") item
    }
    next
}
FILENAME == ARGV[3] {   # the README
    lines[++nl] = $0
    if ($0 ~ /^<!-- stunt-parity:begin/) { inside = 1; begin = nl; next }
    if ($0 ~ /^<!-- stunt-parity:end/) { inside = 0; end = nl; next }
    if (!inside) next
    if ($0 ~ /^\| `test_stunt_/) {
        row = $0; gsub(/\\\|/, "\001", row)    # an escaped | inside a policy
        n = split(row, cell, "|")
        t = trim(cell[2]); gsub(/`/, "", t)
        for (c = 2; c <= 4; c++) old[t, c] = trim(cell[c + 2])
    } else if ($0 ~ /^- Column [234], /) {
        key = substr($0, 1, index($0, ":") - 1)
        oldprov[key] = substr($0, index($0, ":") + 2)
    }
    next
}
END {
    while ((getline line < provfile) > 0) host = line
    for (c in ran) {
        side = ran[c]
        for (i = 1; i <= ntests; i++) {
            t = tname[i]
            # A test with no row stunt runs this way (nested: in-process only).
            if (!seen[t, c]) { cur[t, c] = "(no row of it runs this way)"; continue }
            d = diff[t, c]; gsub(/\n/, ", ", d)
            cur[t, c] = d == "" ? "matches" : "differs: " d
        }
    }
    nran = 0; for (c in ran) nran++
    if (nran == 0) {
        o = ""; for (c in other) o = o (o == "" ? "" : ", ") c
        printf "  [SKIP] parity: no column of the table runs on this host (ran: %s)\n", o
        exit 0
    }
    if (what == "check") {
        if (!begin || !end) { print "  [FAIL] parity: no Stunt parity section in the README; sh tests/run.sh --write-parity"; exit 1 }
        bad = 0; checked = 0
        for (c in ran) for (i = 1; i <= ntests; i++) {
            t = tname[i]
            if (!((t, c) in cur)) continue
            checked++
            want = sidevalue(old[t, c], ran[c])
            if (want != cur[t, c]) {
                printf "  [FAIL] parity: %s, column %s (ae %s %s): the README says \"%s\", this run \"%s\"\n", t, c, ran[c], pin, want, cur[t, c]
                bad++
            }
        }
        cols = ""; for (c = 2; c <= 4; c++) if (c in ran) cols = cols (cols == "" ? "" : ", ") c
        if (bad) { print "  (sh tests/run.sh --write-parity rewrites it from this run)"; exit 1 }
        printf "  [PASS] parity: the README'"'"'s Stunt parity table agrees with this run, %d cells in column(s) %s\n", checked, cols
        exit 0
    }
    # write
    for (c = 2; c <= 4; c++) for (i = 1; i <= ntests; i++) {
        t = tname[i]
        o = old[t, c]
        if (!((t, c) in cur)) { new[t, c] = o; continue }
        f = sidevalue(o, "from"); b = sidevalue(o, "before")
        if (ran[c] == "from") { f = cur[t, c]; if (b == "") b = f } else { b = cur[t, c]; if (f == "") f = b }
        new[t, c] = compose(f, b)
    }
    for (c in ran) oldprov["- Column " c ", ae " ran[c] " " pin] = host
    sec = "<!-- stunt-parity:begin: generated by sh tests/run.sh --write-parity, from build/parity-*.tsv -->\n"
    sec = sec "| stunt test | policy | 2. Sandbox block, no preload | 3. Under `LD_PRELOAD` | 4. Contained child |\n"
    sec = sec "|---|---|---|---|---|\n"
    for (i = 1; i <= ntests; i++) {
        t = tname[i]
        sec = sec "| `" t "` | " tpol[t] " | " new[t, 2] " | " new[t, 3] " | " new[t, 4] " |\n"
    }
    sec = sec "\n"
    for (c = 2; c <= 4; c++) for (s = 1; s <= 2; s++) {
        key = "- Column " c ", ae " (s == 1 ? "from" : "before") " " pin
        if (key in oldprov) sec = sec key ": " oldprov[key] "\n"
    }
    sec = sec "<!-- stunt-parity:end -->"
    if (!begin || !end) {
        print "  [FAIL] parity: no <!-- stunt-parity:begin --> / <!-- stunt-parity:end --> markers in the README to write between"
        exit 1
    }
    for (i = 1; i < begin; i++) print lines[i] > out
    print sec > out
    for (i = end + 1; i <= nl; i++) print lines[i] > out
    close(out)
    print "  [PASS] parity: wrote the Stunt parity table, columns run here updated"
}
' "$TESTS" "$RESULTS" "$README" || exit 1
if [ "$what" = write ]; then
    cp "$out" "$README"
fi
