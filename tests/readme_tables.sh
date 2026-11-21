#!/bin/sh
# The README's second table (what each language is, where it runs, what CI
# does with it), with the two columns a test can say made from the sources
# instead of typed:
#
#   tests/readme_tables.sh check    fail when the README's table disagrees with them
#   tests/readme_tables.sh write    rewrite those two columns in place
#
#   "CI today"      .github/workflows/ci.yml: every job (all run the current ae, AE_PIN) gives its runner, the job's GHL_CI_RUNTIME label, and
#                   "+ child" when it runs with GHL_REQUIRE_CHILD=1. tests/run.sh
#                   checks the label against the runtime it found, on Actions.
#   "Containment"   <lang>/README.md's Stunt parity table (itself written from
#                   runs by tests/run.sh --write-parity): per way, "all match",
#                   or how many of the stunt tests have a row the mode lets
#                   through ("not held", "escapes"), or that the runtime does
#                   not start there. Rows left out or stricter are not counted.
#
# The other columns are hand-written and are kept as they are. Rows are found by
# the link in the first cell, (<lang>/README.md...). Needs python3.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
what="${1:-check}"
case "$what" in check|write) ;; *) echo "usage: readme_tables.sh check|write"; exit 2 ;; esac
command -v python3 > /dev/null 2>&1 || { echo "  [SKIP] readme tables: no python3"; exit 0; }
exec python3 -I - "$what" "$ROOT" <<'PY'
import re, sys
what, root = sys.argv[1], sys.argv[2]

# --- CI today, from the workflow -------------------------------------------
ci = open(root + "/.github/workflows/ci.yml").read()
runners = {"ubuntu-24.04": "Ubuntu 24.04", "ubuntu-24.04-arm": "Ubuntu 24.04 arm64", "macos-15": "macOS 15"}
today = {}   # lang -> list of (runner, label, child)
for job in re.split(r"\n(?=  [a-z0-9-]+:\n)", ci)[1:]:
    langs = re.findall(r"GUEST_LANG=(\w+)", job)
    if not langs:
        continue
    # The ae of the job: the toolchain action installs AE_PIN.
    if "uses: ./.github/actions/toolchain" not in job:
        continue
    ro = re.search(r"runs-on: (\S+)", job).group(1)
    lab = re.search(r'GHL_CI_RUNTIME: "([^"]*)"', job)
    if lab is None:
        print("  [FAIL] readme tables: a job on AE_PIN for %s has no GHL_CI_RUNTIME" % langs[0])
        sys.exit(1)
    child = "GHL_REQUIRE_CHILD=1" in job
    today.setdefault(langs[0], []).append((runners.get(ro, ro), lab.group(1), child))

def ci_cell(lang):
    jobs = today.get(lang, [])
    parts = []
    for r, l, c in sorted(jobs):
        parts.append("%s: %s%s" % (r, l, " + child" if c else ""))
    return "<br>".join(parts) if parts else "none"

# --- Containment, from each language's parity table ------------------------
def containment_cell(lang):
    text = open("%s/%s/README.md" % (root, lang)).read()
    m = re.search(r"<!-- stunt-parity:begin.*?-->\n(.*?)<!-- stunt-parity:end", text, re.S)
    if not m:
        return "no parity table"
    rows = [l for l in m.group(1).split("\n") if l.startswith("| `test_stunt_")]
    cols = {2: [], 3: [], 4: []}
    for r in rows:
        cells = [c.strip() for c in r.replace("\\|", "\x01").split("|")]
        # ['', test, policy, c2, c3, c4, '']
        for i, c in zip((2, 3, 4), cells[3:6]):
            cols[i].append(c.replace("\x01", "|"))
    names = {2: "macOS", 3: "Linux preload", 4: "child"}
    out = []
    for i in (2, 3, 4):
        cs = [c for c in cols[i] if not c.startswith("(no row")]
        n = len(cs)
        blocked = sum(1 for c in cs if "does not start" in c)
        notrun = sum(1 for c in cs if c.startswith("not run") or c.startswith("does not run"))
        # "not held" and "escapes" are rows the mode lets through that the
        # policy refuses; "left out" and "stricter" are not weaknesses.
        weak = sum(1 for c in cs if c.startswith("differs:") and "does not start" not in c and re.search(r"not held|escapes|PRELOAD FAILS", c))
        if blocked == n:
            t = "runtime does not start"
        elif notrun == n:
            t = "not run"
        elif weak == 0 and blocked == 0:
            t = "every row held" if notrun == 0 else "every row that runs held"
        else:
            t = "%d of %d tests have a row not held" % (weak + blocked, n)
        out.append("%s: %s" % (names[i], t))
    return "; ".join(out)

# --- Boundary, from each language's boundary table (tests/boundary_table.py) --
sys.path.insert(0, root + "/tests")
import boundary_table as bt

GROUPS = [("str", ["str", "str-empty", "str-nonascii", "str-nul", "str-60k"], {"str-empty": "empty", "str-nonascii": "non-ASCII", "str-nul": "NUL", "str-60k": "60 KB"}),
          ("int", ["int", "int-neg"], {"int-neg": "negative"}), ("int64", ["int-max", "int-min"], {}),
          ("float", ["float"], {}), ("bool", ["bool-true", "bool-false"], {}), ("null", ["null"], {}),
          ("array", ["array"], {}), ("map", ["map"], {}), ("nested", ["nested"], {})]

def types_dir(res, d):
    ok, partial, no = [], [], []
    for name, ids, labels in GROUPS:
        got = [res.get((i, d)) == "ok" for i in ids]
        if name == "int64":
            continue
        if name == "int":
            i64 = [res.get((i, d)) == "ok" for i in ("int-max", "int-min")]
            if all(got) and not all(i64):
                partial.append("int (not 64-bit)"); continue
            if not all(got):
                (partial if any(got) else no).append("int" if not any(got) else "int (some)"); continue
            ok.append("int"); continue
        if all(got):
            ok.append(name)
        elif any(got):
            miss = [labels.get(i, i) for i, g in zip(ids, got) if not g]
            partial.append("%s (not %s)" % (name, ", ".join(miss)))
        else:
            no.append(name)
    return ok + partial, no

def types_cell(res):
    up, upno = types_dir(res, "up"); dn, dnno = types_dir(res, "down")
    def one(l, no):
        s = ", ".join(l) if l else "none"
        return s + ("; not " + ", ".join(no) if no and l else "")
    if (up, upno) == (dn, dnno):
        return "both ways: " + one(up, upno)
    return "guest to host: %s<br>host to guest, passed back: %s" % (one(up, upno), one(dn, dnno))

def duplex_cell(res):
    if not any(v == "ok" for k, v in res.items() if k[0] in ("str", "int")):
        return "no host calls cross"
    nest = "calls nest in an expression" if res.get(("nest", "-")) == "ok" else "nested calls: " + res.get(("nest", "-"), "n/a")
    loop, wait = res.get(("loop", "-"), "n/a"), res.get(("wait", "-"), "n/a")
    if loop == "heard in a loop":
        cancel = "cancel heard in a loop and at a call"
    elif wait == "heard at the call":
        cancel = "cancel heard only at a call (a loop is killed after the grace period)"
    else:
        cancel = "cancel: loop %s, call %s" % (loop, wait)
    return "guest starts every exchange, host answers or cancels; %s; %s" % (nest, cancel)

def callbacks_cell(res):
    if not any(v == "ok" for k, v in res.items() if k[0] in ("str", "int")):
        return "none: no host function is called"
    r = res.get(("refuse", "-"), "n/a")
    return "by name; argument and return types as in types; a `FAIL` answer: " + r

def transport_cell(res, extra):
    s60 = "a 60 KB str crosses" if res.get(("str-60k", "up")) == "ok" else "a 60 KB str does not cross"
    return "%s%s; a 100 KB argument: %s; a 100 KB answer: %s" % (extra + "; " if extra else "", s60, res.get(("big-up", "-"), "n/a"), res.get(("big-down", "-"), "n/a"))

TRANSPORT_EXTRA = {"go": "the worker is a Go program: its script is `call NAME [JSON]` lines, values are the JSON text"}
# Languages with no boundary table: what the tests say instead, checked below.
STATIC = {
    "java": ("blocked", "does not start: the preload refuses the JVM's executable memory"),
    "factor": ("blocked", "does not start: the preload refuses Factor's executable memory"),
    "racket": ("notrun", "does not start: Chez Scheme cannot run in the child"),
    "tinygo": (None, None),
}

def boundary_cells(lang):
    """(transport, types, duplex, callbacks) or None when the language has no table."""
    text = open("%s/%s/README.md" % (root, lang)).read()
    res = bt.parse_readme_table(text)
    if res is None:
        return None
    return (transport_cell(res, TRANSPORT_EXTRA.get(lang, "")), types_cell(res), duplex_cell(res), callbacks_cell(res))

def static_cells(lang):
    kind, why = STATIC.get(lang, (None, None))
    if lang == "tinygo":
        return ("a `std.guest` worker that loads a Go library; the script is lines `call SYMBOL a<TAB>b`",
                "str only: two strings in, one string out; no JSON", "the script's lines run in order; the host cancels (checked by the `spin` row)",
                "none: the worker mounts no host function")
    return (why, why, why, why)

def static_ok(lang):
    """The claim 'does not start' is a parity-table fact: check it."""
    kind, why = STATIC.get(lang, (None, None))
    text = open("%s/%s/README.md" % (root, lang)).read()
    m = re.search(r"<!-- stunt-parity:begin.*?-->\n(.*?)<!-- stunt-parity:end", text, re.S)
    rows = [l for l in (m.group(1) if m else "").split("\n") if l.startswith("| `test_stunt_")]
    c4 = [r.replace("\\|", "\x01").split("|")[5].strip() for r in rows]
    c4 = [c for c in c4 if not c.startswith("(no row")]
    if kind == "blocked":
        return all("does not start" in c for c in c4)
    if kind == "notrun":
        return all(c.startswith("not run") or c.startswith("does not run") for c in c4) or all("does not start" in c for c in c4)
    return True

# --- the README -------------------------------------------------------------
path = root + "/README.md"
lines = open(path).read().split("\n")
b = next((i for i, l in enumerate(lines) if l.startswith("<!-- language-facts:begin")), None)
e = next((i for i, l in enumerate(lines) if l.startswith("<!-- language-facts:end")), None)
if b is None or e is None:
    print("  [FAIL] readme tables: no language-facts markers in README.md")
    sys.exit(1)
header = next(i for i in range(b, e) if lines[i].startswith("| Language"))
cols = [c.strip() for c in lines[header].strip("|").split("|")]
ci_i = cols.index("CI today (generated)")
co_i = cols.index("Containment (generated)")
bad = 0
for i in range(header + 2, e):
    if not lines[i].startswith("|"):
        continue
    cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
    m = re.search(r"\((\w+)/README\.md", cells[0])
    if not m:
        print("  [FAIL] readme tables: row %d has no link to <lang>/README.md" % (i + 1))
        bad += 1
        continue
    lang = m.group(1)
    want = {ci_i: ci_cell(lang), co_i: containment_cell(lang)}
    for ix, w in want.items():
        if cells[ix] != w:
            if what == "check":
                print("  [FAIL] readme tables: %s, column %r: the README says %r, the sources say %r" % (lang, cols[ix], cells[ix], w))
                bad += 1
            cells[ix] = w
    lines[i] = "| " + " | ".join(cells) + " |"
# --- the boundary table: the child columns are generated -----------------------
bb = next((i for i, l in enumerate(lines) if l.startswith("<!-- language-boundary:begin")), None)
be = next((i for i, l in enumerate(lines) if l.startswith("<!-- language-boundary:end")), None)
if bb is None or be is None:
    print("  [FAIL] readme tables: no language-boundary markers in README.md")
    sys.exit(1)
bh = next(i for i in range(bb, be) if lines[i].startswith("| Language"))
bcols = [c.strip() for c in lines[bh].strip("|").split("|")]
gen = ["Child: transport (generated)", "Child: types (generated)", "Child: duplex (generated)", "Child: callbacks (generated)"]
gi = [bcols.index(g) for g in gen]
for i in range(bh + 2, be):
    if not lines[i].startswith("|"):
        continue
    cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
    m = re.search(r"\((\w+)/README\.md", cells[-1])
    if not m:
        print("  [FAIL] readme tables: boundary row %d has no link to <lang>/README.md" % (i + 1))
        bad += 1
        continue
    lang = m.group(1)
    want = boundary_cells(lang)
    if want is None:
        want = static_cells(lang)
        if not static_ok(lang):
            print("  [FAIL] readme tables: %s: the README table says its child does not start, and its parity table says otherwise" % lang)
            bad += 1
    for ix, w in zip(gi, want):
        if cells[ix] != w:
            if what == "check":
                print("  [FAIL] readme tables: %s, column %r: the README says %r, the tests say %r" % (lang, bcols[ix], cells[ix], w))
                bad += 1
            cells[ix] = w
    lines[i] = "| " + " | ".join(cells) + " |"

if what == "write":
    open(path, "w").write("\n".join(lines))
    print("  [PASS] readme tables: wrote the generated columns")
elif bad:
    print("  (sh tests/readme_tables.sh write rewrites them)")
    sys.exit(1)
else:
    print("  [PASS] readme tables: the generated columns agree with ci.yml and the parity tables")
PY
