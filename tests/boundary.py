#!/usr/bin/env python3
"""The shared boundary tests, run against one language's contained child.

  boundary.py run <lang> <child_host> <workdir> <out.tsv>
  boundary.py run-inproc <lang> <boundary_host> <workdir> <out.tsv>
  boundary.py run-invoke <lang> <child_host> <workdir> <out.tsv>

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

run-invoke is the other direction, the host calling the guest (std.guest's
INVOKE, the host in tests/lib/invoke): the language's x: snippets (a script that
exports functions, written in that language) are run with a plan of invocations,
and what came back is judged: every type as a RETURN value and as an INVOKE
argument handed back, the errors, the cancel, the limits, the notify-then-fetch
pattern, and the host-calls-guest safety rows by name (INVOKE_ROWS).
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

# ---- the other direction: the host calls the guest ---------------------------

# The safety rows of the host-calls-guest verb (aether's
# docs/design/guest-host-calls-guest.md), held by name in every language that
# exports, as stunt's rows are. stunt's catalogue does not carry them (its child
# mode speaks a protocol of its own, not std.guest's), so they are this
# repository's, beside the catalogue.
INVOKE_ROWS = ["guest-export-called", "guest-export-slow", "guest-export-silent", "guest-export-oversize",
               "guest-export-raises", "guest-reenters-while-called", "guest-export-flood",
               "guest-export-malformed", "guest-after-end"]

def run_plan(host, work, script, plan, give="null", upper=False, timeout=60):
    env = dict(os.environ, GHL_BOUNDARY_SCRIPT=script, GHL_BOUNDARY_GIVE=give, GHL_BOUNDARY_UPPER="1" if upper else "0",
               GHL_INVOKE_PLAN="\n".join(plan))
    try:
        p = subprocess.run([host, "-", "invoke", "x"], cwd=work, env=env, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return [("END", "HUNG ")]
    data = p.stdout
    if os.environ.get("GHL_BOUNDARY_DEBUG"):
        sys.stderr.write("--- %s\n%s\n%s\n" % (script[:300], data[:1500].decode("utf-8", "replace"), p.stderr[-600:].decode("utf-8", "replace")))
    ev, i = [], 0
    while i < len(data):
        sp = data.find(b" ", i)
        sp2 = data.find(b" ", sp + 1) if sp > 0 else -1
        try:
            tag = data[i:sp].decode("ascii")
            n = int(data[sp + 1:sp2])
        except Exception:
            i = data.find(b"\n", i) + 1
            if i <= 0: break
            continue
        ev.append((tag, data[sp2 + 1:sp2 + 1 + n].decode("utf-8", "replace")))
        i = sp2 + 1 + n + 1
    if b"Sanitizer" in p.stderr:
        # quickjs/sanitize.sh: a report from the worker (uncontained) or the host fails the case and is shown.
        sys.stdout.write(p.stderr.decode("utf-8", "replace")[-3000:])
        ev.append(("END", "SANITIZER report on stderr"))
    if not any(t == "END" for t, _ in ev):
        why = "signal %d" % -p.returncode if p.returncode < 0 else "no END"
        ev.append(("END", "CRASHED %s: %s" % (why, (data + p.stderr)[-150:].decode("utf-8", "replace").strip())))
    return ev

def outcomes(ev):
    """The answers to the plan's invocations, in order: (kind, text), RETURN/RAISE/NOSEND."""
    return [(t, b) for t, b in ev if t in ("RETURN", "RAISE", "NOSEND", "SENT")]

def end_of(ev):
    k, _, t = [b for tg, b in ev if tg == "END"][-1].partition(" ")
    return k, t

def same_json(a, b):
    try:
        return canon(a) == canon(b)
    except Exception:
        return False

def why_not(ev, n=60):
    """What happened instead, for a cell that is not ok."""
    out = outcomes(ev)
    if out and out[-1][0] == "RAISE": return "raised: " + short(out[-1][1], n)
    if out and out[-1][0] == "RETURN": return "returned " + short(out[-1][1], n)
    k, t = end_of(ev)
    if not any(tg == "READY" for tg, _ in ev): return "no READY: %s %s" % (k, short(t, n))
    return "%s %s" % (k.lower(), short(t, n))

def main_invoke(lang, host, work, out):
    sn = snippets(lang)
    up = sn.get("names") == "upper"
    res = []
    def line(i, d, o, detail=""): res.append("%s\t%s\t%s\t%s" % (i, d, o, detail.replace("\t", " ").replace("\n", " ")))
    def run(key, plan, give="null", v=None):
        script = sn[key] if v is None else sn[key].replace("@V@", v)
        return run_plan(host, work, expand(script), plan, give, up)
    def I(name, args): return "I\t%s\t%s" % (name, args)
    for cid, w in cases():
        v = sn.get("v:" + cid)
        if v is not None and "x:make" in sn:
            ev = run("x:make", [I("make", "[]")], v=v)
            o = outcomes(ev)
            if o and o[0][0] == "RETURN" and same_json(o[0][1], w): line(cid, "ret", "ok")
            elif o and o[0][0] == "RETURN": line(cid, "ret", "differs", "the host saw " + short(o[0][1]))
            else: line(cid, "ret", "error", why_not(ev))
        else:
            line(cid, "ret", "n/a")
        if "x:back" in sn:
            ev = run("x:back", [I("back", "[%s]" % w)])
            o = outcomes(ev)
            if o and o[0][0] == "RETURN" and same_json(o[0][1], w): line(cid, "arg", "ok")
            elif o and o[0][0] == "RETURN": line(cid, "arg", "differs", "the host saw " + short(o[0][1]))
            else: line(cid, "arg", "error", why_not(ev))
        else:
            line(cid, "arg", "n/a")
    def special(key, snip, plan, judge, give="null"):
        if snip not in sn:
            line(key, "-", "n/a"); return None
        ev = run(snip, plan, give)
        o, d = judge(ev); line(key, "-", o, d)
        return o
    ok = lambda o: o in ("ok",)
    # An export that calls a mounted host function, mid-invocation.
    def j_relay(ev):
        o = outcomes(ev)
        wires = [b for t, b in ev if t == "WIRE"]
        if wires and wires[0].replace(" ", "") == '["hello"]' and o and o[0] == ("RETURN", '"hello"'): return "ok", ""
        return "differs", why_not(ev)
    special("relay", "x:relay", [I("relay", '["hello"]')], j_relay)
    # State between invocations, and an export that raises.
    def j_state(ev):
        o = outcomes(ev)
        if [b for t, b in o if t == "RETURN"][:3] == ["1", "2", "3"]: return "state carries", ""
        return "differs", " ".join("%s %s" % (t, short(b, 20)) for t, b in o)
    special("state", "x:count", [I("count", "[]")] * 3, j_state)
    raise_text = [""]
    def j_raise(ev):
        o = outcomes(ev)
        if len(o) == 3 and o[0] == ("RETURN", "1") and o[1][0] == "RAISE" and o[2] == ("RETURN", "2"):
            raise_text[0] = o[1][1]
            return "RAISE: " + short(o[1][1], 70), ""
        return "differs", " ".join("%s %s" % (t, short(b, 30)) for t, b in o) + " " + " ".join(end_of(ev))
    special("raise", "x:count", [I("count", "[]"), I("boom", "[]"), I("count", "[]")], j_raise)
    def j_cancel(ev):
        k, t = end_of(ev)
        if k == "ERROR" and "cancelled by host" in t: return "heard in a loop", ""
        if k == "KILLED" and "no answer to CANCEL" in t: return "not heard: killed", ""
        return "other", short(k + " " + t)
    special("loop", "x:loop", [I("spin", "[]")], j_cancel)
    def j_wait(ev):
        k, t = end_of(ev)
        if k == "ERROR" and "cancelled by host" in t: return "heard at the call", ""
        if k == "KILLED": return "not heard: killed", short(t)
        return "other", short(k + " " + t)
    special("wait", "x:wait", [I("wait", "[]")], j_wait)
    def j_big(ev):
        k, t = end_of(ev)
        if k == "KILLED" and "frame refused" in t: return "refused, worker killed", ""
        o = outcomes(ev)
        if o and o[0][0] == "RETURN": return "crossed", ""
        return "other", why_not(ev)
    special("big", "x:big", [I("big", "[]")], j_big)
    # A direct call carries its whole input: several arguments, and one past
    # the frame limit (the host's to refuse or the worker's to fail on).
    def j_pick(ev):
        o = outcomes(ev)
        if o and o[0] == ("RETURN", '"two"'): return "ok", ""
        return "differs", why_not(ev)
    special("args", "x:pick", [I("pick", '[1,"two",3.5]')], j_pick)
    def j_bigarg(ev):
        o = outcomes(ev)
        if o and o[0][0] == "NOSEND": return "refused by the host, nothing sent", short(o[0][1])
        if o and o[0][0] == "RETURN": return "crossed", ""
        k, t = end_of(ev)
        return "refused: " + k.lower(), short(t)
    special("big-arg", "x:back", [I("back", json.dumps(["x" * 100000]))], j_bigarg)
    def j_unexported(ev):
        o = outcomes(ev)
        if o and o[0][0] == "NOSEND" and "does not export" in o[0][1] and len(o) > 1 and o[1][0] == "RETURN":
            return "refused by the host, nothing sent; the guest serves on", ""
        return "differs", " ".join("%s %s" % (t, short(b, 30)) for t, b in o)
    special("unexported", "x:back", [I("no_such_export", "[]"), I("back", "[1]")], j_unexported)
    def j_end(ev):
        k, t = end_of(ev)
        if k == "DONE" and t.strip() == "0" and any(tg == "READY" for tg, _ in ev): return "DONE 0", ""
        return "differs", short(k + " " + t)
    special("end", "x:back", ["E"], j_end)
    def j_none(ev):
        k, t = end_of(ev)
        if not any(tg == "READY" for tg, _ in ev) and k == "DONE": return "DONE, as before exports", ""
        return "differs", short(k + " " + t)
    special("none", "x:none", [I("back", "[1]")], j_none)
    # Notify, then fetch: the script starts two transactions (txn_start answers
    # an id at once) and exports on_txn; the host later invokes on_txn with an id
    # only, and the export fetches the details with txn_details, keeping a count
    # between notifications. An unknown id: txn_details fails, and the export
    # handles it or raises.
    def j_txn(ev):
        o = outcomes(ev)
        starts = [b for t, b in ev if t == "CALL"][:2]
        want = [("RETURN", '"1 deposit 100"'), ("RETURN", '"2 refund 200"')]
        if starts != ["txn_start", "txn_start"] or len(o) != 4 or o[:2] != want or o[3] != ("RETURN", '"3 deposit 100"'):
            return "differs", " ".join("%s %s" % (t, short(b, 30)) for t, b in o) + " " + " ".join(end_of(ev))
        if o[2][0] == "RAISE" and "no transaction 999999" in o[2][1]: return "ok; an unknown id raises: " + short(o[2][1], 70), ""
        if o[2][0] == "RETURN": return "ok; an unknown id is handled: returns " + short(o[2][1], 40), ""
        return "differs", "unknown id: %s %s" % o[2]
    special("txn", "x:txn", [I("on_txn", '["123456"]'), I("on_txn", '["123457"]'), I("on_txn", '["999999"]'),
                             I("on_txn", '["123456"]')], j_txn)
    # The safety rows, by name.
    def held_if(cond, ev, stricter=None):
        if cond: return "held", ""
        if stricter: return "stricter: " + stricter, ""
        return "not held", why_not(ev)
    def r_called(ev):
        o = outcomes(ev); k, t = end_of(ev)
        return held_if(o == [("RETURN", '"x"')] and k == "DONE" and t.strip() == "0", ev)
    special("guest-export-called", "x:back", [I("back", '["x"]'), "E"], r_called)
    def r_slow(ev):
        k, t = end_of(ev)
        return held_if(k == "ERROR" and "cancelled by host" in t, ev,
                       "killed after the grace period (the cancel is not heard in a loop)" if k == "KILLED" and "no answer to CANCEL" in t else None)
    special("guest-export-slow", "x:loop", [I("spin", "[]")], r_slow)
    def r_silent(ev):
        k, t = end_of(ev)
        if k == "ERROR" and "cancelled by host" in t: return "left out: the export cannot stop the worker hearing the cancel", ""
        return held_if(k == "KILLED" and "no answer to CANCEL" in t, ev)
    if "x:silent" in sn:
        special("guest-export-silent", "x:silent", [I("hush", "[]")], r_silent)
    else:
        line("guest-export-silent", "-", "left out", sn.get("why:silent", "the language cannot ignore a cancel"))
    def r_oversize(ev):
        k, t = end_of(ev)
        return held_if(k == "KILLED" and "frame refused" in t, ev)
    special("guest-export-oversize", "x:big", [I("big", "[]")], r_oversize)
    def r_raises(ev):
        o = outcomes(ev)
        return held_if(len(o) == 2 and o[0][0] == "RAISE" and o[1] == ("RETURN", "1"), ev)
    special("guest-export-raises", "x:count", [I("boom", "[]"), I("count", "[]")], r_raises)
    def r_reenter(ev):
        o = outcomes(ev)
        return held_if(len(o) == 2 and o[0][0] == "NOSEND" and "not serving" in o[0][1] and o[1] == ("RETURN", '"x"'), ev)
    special("guest-reenters-while-called", "x:reenter", [I("via", '["x"]')], r_reenter)
    def r_readybad(ev):
        k, t = end_of(ev)
        if k == "KILLED" and "protocol: READY" in t: return "held", ""
        if k == "ERROR": return "stricter: the worker refuses it: " + short(t, 60), ""
        return "not held", why_not(ev)
    special("guest-export-flood", "x:flood", [I("f1", "[]")], r_readybad)
    special("guest-export-malformed", "x:malformed", [I("back", "[]")], r_readybad)
    def r_after(ev):
        o = outcomes(ev); k, t = end_of(ev)
        return held_if(o[:1] == [("RETURN", "1")] and o[-1][0] == "NOSEND" and "finished" in o[-1][1] and k == "DONE", ev)
    special("guest-after-end", "x:back", [I("back", "[1]"), "E", "A\tback\t[1]"], r_after)
    open(out, "w", encoding="utf-8").write("\n".join(res) + "\n")

def main_bench(lang, host, work, n="10000"):
    """One line per mode and size: microseconds per call, contained."""
    sn = snippets(lang)
    up = sn.get("names") == "upper"
    for size in (16, 60000):
        row = []
        for mode, key in (("g2h", "x:bench-g2h"), ("h2g", "x:back"), ("pull", "x:bench-pull")):
            if key not in sn:
                row.append("n/a"); continue
            script = expand(sn[key].replace("@N@", n).replace("@SIZE@", str(size)))
            env = dict(os.environ, GHL_BOUNDARY_SCRIPT=script, GHL_BOUNDARY_UPPER="1" if up else "0",
                       GHL_INVOKE_BENCH="%s %s %d" % (mode, n, size), GHL_INVOKE_PLAN="")
            p = subprocess.run([host, "-", "invoke", "x"], cwd=work, env=env, capture_output=True, timeout=900)
            out = p.stdout.decode("utf-8", "replace")
            m = re.search(r"^BENCH \d+ \S+ \d+ \d+ (\d+)$", out, re.M)
            row.append(m.group(1) if m else "failed: " + short(out.strip() + p.stderr.decode("utf-8", "replace")[-200:], 120))
        print("%s\t%d\t%s" % (lang, size, "\t".join(row)))

if __name__ == "__main__":
    if len(sys.argv) in (5, 6) and sys.argv[1] == "bench":
        main_bench(*sys.argv[2:])
    elif len(sys.argv) == 6 and sys.argv[1] == "run-invoke":
        main_invoke(*sys.argv[2:])
    elif len(sys.argv) == 6 and sys.argv[1] == "run":
        main_run(*sys.argv[2:])
    elif len(sys.argv) == 6 and sys.argv[1] == "run-inproc":
        main_run(*sys.argv[2:], inproc=True)
    else:
        print(__doc__); sys.exit(2)
