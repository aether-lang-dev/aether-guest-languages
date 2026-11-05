// The rows of stunt's catalogue (aether contrib/host/stunt) that Duktape can
// express, each done the way a JavaScript script would do it, and the judge
// that turns what happened into stunt's one line:
//
//   <check>: <outcome> (<detail>) - expected <expectation>: PASS|FAIL
//
// The host (host.ae) prepends a few facts it found before the sandbox was in
// force (HOST_HOME_SET, HOST_SH, HOST_EXISTS, HOST_CWD_WRITABLE), appends
// `check_line(name, expectation)` and reads the line back. ES5.1 only: no
// let, const, arrow functions or template strings.
//
// Duktape has no I/O of its own. What a script can reach is what the bridge
// gives it as globals (print, env, readFile, fileExists, writeFile, exec),
// which check the sandbox's grants themselves and answer undefined, false or
// -1 for a refusal, and the host functions the host registers (read_file,
// getenv, greet, add, aether_map_get/put), which throw an Error for a
// refusal. So a refusal is an answer that says no where the host has
// evidence the thing is there: the file exists, HOME is set, /bin/sh is
// there. Anything else that goes wrong is an error, never a refusal, so it
// cannot pass for a sandbox at work. The rows Duktape cannot express are
// left out by name in lang.sh.
//
// In the contained child (worker/worker.ae) the script is not given env,
// readFile, writeFile, fileExists or exec at all: the first five rows below
// then come out "refused: no <fn>", which is the child being stricter than
// the policy (lang.sh), not an error.

var G = this;

function firstLine(s) {
  return String(s).split('\n')[0].substring(0, 40);
}

function contains(list, item) {
  for (var i = 0; i < list.length; i++) if (list[i] === item) return true;
  return false;
}

// The bridge's own binding, if the script was given it.
function given(fn) {
  return typeof G[fn] === 'function';
}

// readFile(path): the text, or undefined for a refusal or any failure.
function openPath(path) {
  if (!given('readFile')) return ['refused', 'no readFile: this script is not given one'];
  var s = readFile(path);
  if (typeof s === 'string') return ['succeeded', 'readFile read "' + firstLine(s) + '"'];
  if (contains(HOST_EXISTS, path)) return ['refused', 'readFile gave undefined; ' + path + ' exists'];
  return ['error', 'readFile gave undefined and ' + path + ' is not there'];
}

var checks = {};

checks['open-etc-passwd'] = function () { return openPath('/etc/passwd'); };
checks['open-granted-file'] = function () { return openPath('./stunt-fixture.txt'); };

checks['write-granted-file'] = function () {
  if (!given('writeFile')) return ['refused', 'no writeFile: this script is not given one'];
  var ok = writeFile('./stunt-written.txt', 'written by duktape\n');
  if (ok === true) return ['succeeded', 'wrote ./stunt-written.txt'];
  if (typeof HOST_CWD_WRITABLE !== 'undefined') return ['refused', 'writeFile gave false; the directory takes files'];
  return ['error', 'writeFile gave false and the directory takes no files'];
};

// env() is libc's getenv, behind a grant check. An undefined is a refusal
// only with evidence of one: the host saw HOME in its environment before the
// sandbox.
checks['getenv-home'] = function () {
  if (!given('env')) return ['refused', 'no env: this script is not given one'];
  var v = env('HOME');
  if (typeof v === 'string') return ['succeeded', 'HOME=' + v];
  if (typeof HOST_HOME_SET !== 'undefined') return ['refused', 'env gave undefined; HOME is in the environment'];
  return ['error', 'HOME is unset'];
};

// exec() is system(), behind a grant check on the command line: /bin/sh run
// with its standard input at end of file (host_support.c) is the shell
// stunt's row starts. -1 is the bridge refusing, or system() unable to start
// it; 127 is the C library's own report that the shell could not be executed.
checks['exec-bin-sh'] = function () {
  if (!given('exec')) return ['refused', 'no exec: this script is not given one'];
  var rc = exec('/bin/sh');
  if (rc === 0) return ['succeeded', "exec('/bin/sh'): exit 0"];
  if ((rc === -1 || rc === 127) && typeof HOST_SH !== 'undefined') {
    return ['refused', "exec('/bin/sh') answered " + rc + "; /bin/sh is there"];
  }
  return ['error', "exec('/bin/sh') answered " + rc];
};

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
    if (err.indexOf('nope') >= 0 && err.indexOf('undefined') >= 0) {
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
  aether_map_put('output', 'from-duktape');
  var v = aether_map_get('output');
  if (v !== 'from-duktape') return ['error', 'output read back as ' + v];
  return ['succeeded', 'output = "from-duktape"'];
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
