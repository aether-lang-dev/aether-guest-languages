#!/usr/bin/env python3
"""The shared boundary tests, run against one language's contained child.

  boundary.py run <lang> <child_host> <workdir> <out.tsv>
  boundary.py run-inproc <lang> <boundary_host> <workdir> <out.tsv>

For each case in tests/boundary/cases.tsv the language's own snippets
(<lang>/containment/boundary.tsv: the value written in that language, and how
that language calls a host function and catches an error) become a script, the
worker runs it in its own process under the host in tests/lib/boundary, and
what crossed is judged. Nothing here lists what a language can do: a case a
language has no snippet for is "n/a", one that runs is judged by the bytes the
worker put on the wire. See tests/lib/boundary/module.ae for the host.

run-inproc does the same against the language's bridge in this process
(<lang>/containment/boundary_host.ae, written in Aether against the bridge's real
API, with tests/lib/inproc): the same cases and the same snippets (or
<lang>/containment/boundary_inproc.tsv where the language has to say them
differently). There is no wire, so "what the host saw" is what the host's typed
readers made of the argument. A case the bridge has no call to express reads n/a
(the host says NOAPI); one that runs and loses something reads "differs" with
what the host saw. There is no cancel in-process: the cancel row is n/a, and the
loop row is run with the bridge's own limit (a host without one says NOAPI).
"""
import json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def canon(text):
    return json.dumps(json.loads(text), sort_keys=True, separators=(",", ":"), ensure_ascii=False)

def expand(s):
    out = ""
    while "@REP:" in s:
        a = s.index("@REP:")
        b = s.index("@", a + 5)
        ch, n = s[a + 5:b].split(":")
        out += s[:a] + ch * int(n)
        s = s[b + 1:]
    return (out + s).replace("@NL@", "\n")

def cases():
    rows = []
    for line in open(os.path.join(ROOT, "tests/boundary/cases.tsv"), encoding="utf-8"):
        line = line.rstrip("\n")
        if line and not line.startswith("#"):
            i, w = line.split("\t", 1)
            if w.startswith("@REP:"):
                w = json.dumps(expand(w))
            rows.append((i, w))
    return rows

def snippets(lang, inproc=False):
    path = os.path.join(ROOT, lang, "containment", "boundary.tsv")
    if inproc and os.path.exists(os.path.join(ROOT, lang, "containment", "boundary_inproc.tsv")):
        path = os.path.join(ROOT, lang, "containment", "boundary_inproc.tsv")
    d = {}
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.rstrip("\n")
            if line and not line.startswith("#"):
                k, v = line.split("\t", 1)
                d[k] = v
    return d

def run_one(host, work, script, give, upper=False, timeout=60, inproc=False, limit=False, direction=""):
    env = dict(os.environ, GHL_BOUNDARY_DIR=direction, GHL_BOUNDARY_SCRIPT=script, GHL_BOUNDARY_GIVE=give, GHL_BOUNDARY_UPPER="1" if upper else "0",
               GHL_BOUNDARY_LIMIT="1" if limit else "0")
    try:
        p = subprocess.run([host] if inproc else [host, "-", "boundary", "x"], cwd=work, env=env, capture_output=True, timeout=timeout)
        out_bytes = p.stdout
        text = p.stdout.decode("utf-8", "replace") + p.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return {"wires": [], "says": [], "gave": 0, "refused": 0, "noapi": [], "end": ("HUNG", ""), "raw": ""}
    if os.environ.get("GHL_BOUNDARY_DEBUG"):
        sys.stderr.write("--- %s\n%s\n" % (script[:200], text[:600]))
    wires, says, end, gave, refused, noapi = [], [], ("NONE", text.strip()[-200:]), 0, 0, []
    if p.returncode < 0: end = ("CRASHED", "signal %d: %s" % (-p.returncode, text.strip()[-150:]))
    raw = text
    data = out_bytes
    i = 0
    while i < len(data):
        sp = data.find(b" ", i)
        tag = data[i:sp].decode("ascii", "replace") if sp > 0 else ""
        sp2 = data.find(b" ", sp + 1) if sp > 0 else -1
        try:
            n = int(data[sp + 1:sp2])
        except Exception:
            i = data.find(b"\n", i) + 1
            if i <= 0: break
            continue
        body = data[sp2 + 1:sp2 + 1 + n].decode("utf-8", "replace")
        i = sp2 + 1 + n + 1
        if tag == "WIRE": wires.append(body)
        elif tag == "SAY": says.append(body)
        elif tag == "GIVE": gave += 1
        elif tag == "REFUSE": refused += 1
        elif tag == "NOAPI": noapi.append(body)
        elif tag == "END":
            k, _, t = body.partition(" ")
            end = (k, t)
    return {"wires": wires, "says": says, "gave": gave, "refused": refused, "noapi": noapi, "end": end, "raw": raw}

def short(t, n=60):
    # An address in a message changes from run to run: the README must not.
    t = re.sub(r"0x[0-9a-f]{6,}", "0x...", t)
    return t if len(t) <= n else t[:n] + "..."

def judge_value(r, want, direction):
    if r.get("noapi"): return "n/a", r["noapi"][0]
    if r["wires"]:
        got = r["wires"][0]
        try:
            args = json.loads(got)
            if len(args) == 1 and canon(json.dumps(args[0])) == canon(want):
                return "ok", ""
        except Exception:
            pass
        return "differs", "the host saw " + short(got)
    if direction == "down":
        for s in r["says"]:
            if s.startswith("VALUE "):
                try:
                    if canon(s[6:]) == canon(want): return "ok", "printed"
                except Exception:
                    pass
    k, t = r["end"]
    if k == "CRASHED": return "crashed", short(t)
    if k == "HUNG": return "hung", ""
    # A refusal the language documents: the script is stopped with an error that
    # says "refused: it holds <what>" and nothing reached the host. It is a
    # deliberate outcome, not a defect (tcl/README.md says why Tcl refuses NUL).
    if k == "ERROR" and "refused: it holds" in t: return "refused", short(t)
    if k == "ERROR": return "error", short(t)
    if k == "KILLED": return "killed", short(t)
    return "no call", short(t)

def main_run(lang, host, work, out, inproc=False):
    sn = snippets(lang, inproc)
    up = sn.get("names") == "upper"
    res = []
    def run_one_(script, give, limit=False, direction=""):
        return run_one(host, work, script, give, up, timeout=20 if limit else 60, inproc=inproc, limit=limit, direction=direction)
    def line(i, d, o, detail=""): res.append("%s\t%s\t%s\t%s" % (i, d, o, detail.replace("\t", " ").replace("\n", " ")))
    for cid, w in cases():
        v = sn.get("v:" + cid)
        if v is not None and "up" in sn:
            r = run_one_(expand(sn["up"].replace("@V@", v)), "null", direction="up")
            o, d = judge_value(r, w, "up"); line(cid, "up", o, d)
        else:
            line(cid, "up", "n/a")
        if "down" in sn:
            r = run_one_(expand(sn["down"].replace("@ID@", cid)), w, direction="down")
            o, d = judge_value(r, w, "down"); line(cid, "down", o, d)
        else:
            line(cid, "down", "n/a")
    def special(key, judge, give="null", limit=False):
        if key not in sn:
            line(key, "-", "n/a"); return
        r = run_one_(expand(sn[key]), give, limit)
        if r.get("noapi"): line(key, "-", "n/a", r["noapi"][0]); return
        o, d = judge(r); line(key, "-", o, d)
    def j_nest(r):
        ok = len(r["wires"]) == 2 and all(w.replace(" ", "") == '["n"]' for w in r["wires"])
        return ("ok", "") if ok else ("differs", short(" ".join(r["wires"]) + " " + r["end"][0] + " " + r["end"][1]))
    def j_refuse(r):
        w = [x.replace(" ", "") for x in r["wires"]]
        if r["refused"] and '["caught"]' in w: return "caught", ""
        if r["refused"] and '["returned"]' in w: return "returned as a value", ""
        if r["refused"] and any(s.startswith("FAIL") for s in r["says"]): return "reported as text", ""
        if r["refused"]: return "ends the script", short(r["end"][0] + " " + r["end"][1])
        return "no call", short(r["end"][0] + " " + r["end"][1])
    def j_cancel(heard, killed):
        def j(r):
            k, t = r["end"]
            if k == "ERROR" and "cancelled by host" in t: return heard, ""
            if k == "KILLED": return killed, short(t)
            return "other", short(k + " " + t)
        return j
    def j_bigup(r):
        k, t = r["end"]
        if k == "KILLED" and "frame refused" in t: return "refused, worker killed", ""
        if len(r["wires"]) == 1: return "crossed", ""
        return "other", short(k + " " + t)
    def j_bigdown(r):
        k, t = r["end"]
        if r["wires"]: return "crossed", ""
        return "refused: " + k.lower(), short(t)
    special("nest", j_nest)
    special("refuse", j_refuse)
    def j_limit(r):
        k, t = r["end"]
        if k == "ERROR": return "stopped by the limit", short(t)
        if k == "HUNG": return "not stopped: hung", ""
        if k == "CRASHED": return "crashed", short(t)
        return "not stopped", short(k + " " + t)
    def j_bigup_inproc(r):
        if len(r["wires"]) == 1: return "crossed", ""
        k, t = r["end"]
        return "other", short(k + " " + t)
    if inproc:
        # No cancel in-process: nothing can reach a script that is in a call or a loop
        # except what the bridge itself counts (its limit).
        line("wait", "-", "n/a", "there is no cancel in-process")
        special("loop", j_limit, limit=True)
        special("big-up", j_bigup_inproc)
    else:
        special("wait", j_cancel("heard at the call", "not heard: killed"))
        special("loop", j_cancel("heard in a loop", "not heard: killed"))
        special("big-up", j_bigup)
    if "down" in sn:
        r = run_one_(expand(sn["down"].replace("@ID@", "big")), json.dumps("x" * 100000), direction="down")
        if r.get("noapi"): line("big-down", "-", "n/a", r["noapi"][0])
        elif inproc: o, d = j_bigup_inproc(r); line("big-down", "-", o, d)
        else: o, d = j_bigdown(r); line("big-down", "-", o, d)
    else:
        line("big-down", "-", "n/a")
    open(out, "w", encoding="utf-8").write("\n".join(res) + "\n")

if __name__ == "__main__":
    if len(sys.argv) == 6 and sys.argv[1] == "run":
        main_run(*sys.argv[2:])
    elif len(sys.argv) == 6 and sys.argv[1] == "run-inproc":
        main_run(*sys.argv[2:], inproc=True)
    else:
        print(__doc__); sys.exit(2)
