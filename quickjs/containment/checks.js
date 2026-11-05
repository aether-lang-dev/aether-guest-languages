// The rows of stunt's catalogue (aether contrib/host/stunt) that QuickJS can
// express, each done the way a JavaScript script would do it, and the judge
// that turns what happened into stunt's one line:
//
//   <check>: <outcome> (<detail>) - expected <expectation>: PASS|FAIL
//
// The host (host.ae) prepends a few facts it found before the sandbox was in
// force (HOST_HOME_SET, HOST_EXISTS, ...), appends `check_line(name,
// expectation)` and reads the line back. Plain ES5-style statements run in
// QuickJS's sloppy mode, which is what the bridge runs a script in.
//
// QuickJS has no I/O of its own here: quickjs-libc, which would give a script
// std.open, os.exec and the rest, is not compiled in, and the bridge adds no
// binding that reaches the system (it adds print, console.log and the shared
// map). What a script can reach is the host functions the host registers
// (read_file, getenv, greet, add, and aether_map_get/put), which throw an
// Error for a refusal. So a refusal is an answer that says no where the host
// has evidence the thing is there: the file exists, HOME is set. Anything
// else that goes wrong is an error, never a refusal, so it cannot pass for a
// sandbox at work. The rows QuickJS cannot express are left out by name in
// lang.sh.

var G = globalThis;

function firstLine(s) {
  return String(s).split('\n')[0].substring(0, 40);
}

// The host function if the script was given it.
function given(fn) {
  return typeof G[fn] === 'function';
}

var checks = {};

// A host function's answer: a value, or an Error whose message says why.
function hostStr(fn, arg) {
  if (!given(fn)) return ['error', 'no host function ' + fn];
  var v, why;
  try {
    v = G[fn](arg);
  } catch (e) {
    v = undefined;
    why = String(e && e.message !== undefined ? e.message : e);
  }
  // In the contained child a file comes as std.guest's answer to the call:
  // {granted: true, text: ...} or {granted: false, why: ...}.
  if (v !== null && typeof v === 'object') {
    if (v.granted) v = v.text;
    else { why = 'refused: ' + v.why; v = undefined; }
  }
  if (typeof v === 'string') return ['succeeded', fn + '(' + arg + ') = "' + firstLine(v) + '"'];
  why = String(why);
  if (why.substring(0, 8) === 'refused:') return ['refused', why];
  return ['error', why];
}

// Host functions that check the grants themselves (host.ae).
checks['binding-read-etc-passwd'] = function () { return hostStr('read_file', '/etc/passwd'); };
checks['binding-read-granted-file'] = function () { return hostStr('read_file', './stunt-fixture.txt'); };
checks['binding-getenv-home'] = function () { return hostStr('getenv', 'HOME'); };

checks['callback-greet'] = function () {
  if (!given('greet')) return ['error', 'no host function greet'];
  var v = greet('stunt');
  if (v !== 'hello, stunt') return ['error', 'greet("stunt") = ' + v];
  return ['succeeded', 'greet("stunt") = "hello, stunt"'];
};

checks['callback-add'] = function () {
  if (!given('add')) return ['error', 'no host function add'];
  var v = add(2, 40);
  if (v !== 42) return ['error', 'add(2, 40) = ' + v];
  return ['succeeded', 'add(2, 40) = 42'];
};

// A host function nobody registered is an undefined identifier: calling it
// throws a ReferenceError the script can catch, not a crash.
checks['callback-unregistered'] = function () {
  try {
    nope();
  } catch (e) {
    var err = String(e);
    if (err.indexOf('nope') >= 0 && err.indexOf('not defined') >= 0) {
      return ['refused', 'no host function nope (' + err + ')'];
    }
    return ['error', err];
  }
  return ['succeeded', 'nope() answered'];
};

// The shared map, through the bridge's aether_map_get / aether_map_put.
checks['map-get'] = function () {
  var v = aether_map_get('input');
  if (v === undefined || v === null) return ['error', 'input is not in the map'];
  if (v !== 'stunt-in') return ['error', 'input = "' + v + '"'];
  return ['succeeded', 'input = "stunt-in"'];
};

checks['map-set'] = function () {
  aether_map_put('output', 'from-quickjs');
  var v = aether_map_get('output');
  if (v !== 'from-quickjs') return ['error', 'output read back as ' + v];
  return ['succeeded', 'output = "from-quickjs"'];
};

// aether_map_put gives the in-process script no result, so a refused write is
// seen only by reading the key back: still the host's value.
checks['map-overwrite-input'] = function () {
  aether_map_put('input', 'overwritten');
  var v = aether_map_get('input');
  if (v === 'stunt-in') return ['refused', 'input is still "stunt-in" (aether_map_put gives the script no result)'];
  return ['succeeded', 'input read back as ' + v];
};

function check_line(name, expect) {
  var fn = checks[name];
  if (fn === undefined) {
    return name + ': not expressed in JavaScript - expected ' + expect + ': FAIL';
  }
  var r;
  try {
    r = fn();
  } catch (e) {
    r = ['error', String(e)];
  }
  var pass = (expect === 'succeed' && r[0] === 'succeeded') ||
             (expect === 'refused' && r[0] === 'refused');
  return name + ': ' + r[0] + ' (' + r[1] + ') - expected ' + expect + ': ' + (pass ? 'PASS' : 'FAIL');
}
