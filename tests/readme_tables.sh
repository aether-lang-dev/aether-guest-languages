#!/bin/sh
# The README's second table (what each language is, where it runs, what CI
# does with it), with the two columns a test can say made from the sources
# instead of typed:
#
#   tests/readme_tables.sh check    fail when the README's table disagrees with them
#   tests/readme_tables.sh write    rewrite those two columns in place
#
#   "CI today"      .github/workflows/ci.yml: every job on AE_PIN (the current
#                   ae) gives its runner, the job's GHL_CI_RUNTIME label, and
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
    pins = set(re.findall(r'AE_PIN="\$(\w+)"', job))
    if pins != {"AE_PIN"}:
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
def side_current(cell):
    m = re.match(r"from ae [^:]*: (.*?); before: ", cell)
    return m.group(1) if m else cell

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
            cols[i].append(side_current(c.replace("\x01", "|")))
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
if what == "write":
    open(path, "w").write("\n".join(lines))
    print("  [PASS] readme tables: wrote the generated columns")
elif bad:
    print("  (sh tests/readme_tables.sh write rewrites them)")
    sys.exit(1)
else:
    print("  [PASS] readme tables: the generated columns agree with ci.yml and the parity tables")
PY
