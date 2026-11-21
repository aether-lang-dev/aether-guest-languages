/* The rows of stunt's catalogue (aether contrib/host/stunt) that Rexx can
   express on RexxAE, each done the way a Rexx script would do it, and the
   judge that turns what happened into stunt's one line:

     <check>: <outcome> (<detail>) - expected <expectation>: PASS|FAIL

   The host (host.ae, child_host.ae) puts two lines in front of this,
   LET check = '<row>' and LET expect = '<succeed|refused>', and reads the
   line back from the last SAY (or, in the bridge's map run, from the map).

   RexxAE has no I/O of its own: no STREAM, LINEIN, LINEOUT, CHARIN, CHAROUT,
   no VALUE(name,,'ENVIRONMENT'), no file, environment, process, network or
   library function (test_surface.sh holds that over its whole surface).
   What a script can reach is
     - the functions the host registers (READ_FILE, GETENV, GREET, ADD, and
       AETHER_MAP_GET / AETHER_MAP_PUT), each a host request the host
       answers; READ_FILE and GETENV answer as std.guest answers a file,
       {granted: 1, text: ...} or {granted: 0, why: 'refused: ...'}
     - ADDRESS, also a request to the host: no environment is built in.
   A refusal is an answer whose reason starts "refused:", or an ADDRESS
   environment the host did not mount. Anything else that goes wrong is an
   error, never a refusal. A call to a function nobody registered ends the
   script (RexxAE has no SIGNAL ON to catch it): the host judges that row
   (policy/module.ae, ended_in_error).

   SELECT's branches are multiline WHEN ... THEN bodies with no DO: in
   RexxAE (as in RexxJS) a "THEN DO" on a WHEN is a marker whose END closes
   the SELECT (RexxAE docs/select.md). */

LET outcome = 'error'
LET detail = 'not expressed in Rexx'
LET answered = 0

SELECT
  WHEN check = 'binding-read-etc-passwd' THEN
    LET asked = 'READ_FILE(/etc/passwd)'
    LET r = READ_FILE('/etc/passwd')
    LET answered = 1
  WHEN check = 'binding-read-granted-file' THEN
    LET asked = 'READ_FILE(./stunt-fixture.txt)'
    LET r = READ_FILE('./stunt-fixture.txt')
    LET answered = 1
  WHEN check = 'binding-getenv-home' THEN
    LET asked = 'GETENV(HOME)'
    LET r = GETENV('HOME')
    LET answered = 1
  WHEN check = 'callback-greet' THEN
    LET v = GREET('stunt')
    IF v = 'hello, stunt' THEN DO
      LET outcome = 'succeeded'
      LET detail = 'GREET(stunt) = "hello, stunt"'
    END
    ELSE LET detail = 'GREET(stunt) = ' || v
  WHEN check = 'callback-add' THEN
    LET v = ADD(2, 40)
    IF v = 42 THEN DO
      LET outcome = 'succeeded'
      LET detail = 'ADD(2, 40) = 42'
    END
    ELSE LET detail = 'ADD(2, 40) = ' || v
  WHEN check = 'callback-unregistered' THEN
    /* Ends the script here when nobody registered NOPE. */
    LET v = NOPE()
    LET outcome = 'succeeded'
    LET detail = 'NOPE() answered'
  WHEN check = 'map-get' THEN
    LET v = AETHER_MAP_GET('input')
    IF v = 'stunt-in' THEN DO
      LET outcome = 'succeeded'
      LET detail = 'input = "stunt-in"'
    END
    ELSE LET detail = 'input = ' || v
  WHEN check = 'map-set' THEN
    LET ok = AETHER_MAP_PUT('output', 'from-rexx')
    LET v = AETHER_MAP_GET('output')
    IF v = 'from-rexx' THEN DO
      LET outcome = 'succeeded'
      LET detail = 'output = "from-rexx"'
    END
    ELSE LET detail = 'output read back as ' || v
  WHEN check = 'map-overwrite-input' THEN
    LET ok = AETHER_MAP_PUT('input', 'overwritten')
    LET v = AETHER_MAP_GET('input')
    IF v = 'stunt-in' THEN DO
      LET outcome = 'refused'
      LET detail = 'AETHER_MAP_PUT answered ' || ok || ', input is still "stunt-in"'
    END
    ELSE DO
      LET outcome = 'succeeded'
      LET detail = 'input read back as ' || v
    END
  WHEN check = 'exec-bin-sh' THEN
    /* Rexx starts a program by ADDRESS: a command to an environment the
       host provides. In RexxAE it is always a request to the host. */
    ADDRESS SYSTEM "exit 0"
    IF RC = 0 THEN DO
      LET outcome = 'succeeded'
      LET detail = 'ADDRESS SYSTEM "exit 0": RC 0'
    END
    ELSE IF LEFT(ERRORTEXT, 8) = 'refused:' THEN DO
      LET outcome = 'refused'
      LET detail = 'ADDRESS SYSTEM: RC ' || RC || ', ' || ERRORTEXT
    END
    ELSE IF POS(ERRORTEXT, 'is not mounted') > 0 THEN DO
      LET outcome = 'refused'
      LET detail = 'ADDRESS SYSTEM: RC ' || RC || ', ' || ERRORTEXT
    END
    ELSE LET detail = 'ADDRESS SYSTEM: RC ' || RC || ', ' || ERRORTEXT
  OTHERWISE
    LET detail = 'not expressed in Rexx'
END

/* A host function's answer: granted, refused, or an error. */
IF answered = 1 THEN DO
  IF r.granted THEN DO
    LET outcome = 'succeeded'
    LET detail = asked || ' = "' || WORD(r.text, 1) || '"'
  END
  ELSE IF LEFT(r.why, 8) = 'refused:' THEN DO
    LET outcome = 'refused'
    LET detail = r.why
  END
  ELSE LET detail = asked || ': ' || r.why
END

LET verdict = 'FAIL'
IF expect = 'succeed' THEN DO
  IF outcome = 'succeeded' THEN LET verdict = 'PASS'
END
IF expect = 'refused' THEN DO
  IF outcome = 'refused' THEN LET verdict = 'PASS'
END
LET line = check || ': ' || outcome || ' (' || detail || ') - expected ' || expect || ': ' || verdict
SAY line
