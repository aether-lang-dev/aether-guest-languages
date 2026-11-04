-- The rows of stunt's catalogue (aether contrib/host/stunt) that Lua can
-- express, each done the way a Lua script would do it, and the judge that
-- turns what happened into stunt's one line:
--
--   <check>: <outcome> (<detail>) - expected <expectation>: PASS|FAIL
--
-- The host (host.ae) prepends a few facts it found before the sandbox was
-- in force (HOST_HOME_SET, HOST_SH), appends `return check_line(name,
-- expectation)` and reads the line back. A refusal is what a sandbox gives:
-- EPERM or EACCES from libc, the preload library or the kernel, or a host
-- function's error that starts "refused:". Anything else that goes wrong is
-- an error, never a refusal, so it cannot pass for a sandbox at work. The
-- rows Lua cannot express are left out by name in tests/stunt_lib.sh.

local EPERM, EACCES = 1, 13          -- the same numbers on Linux, macOS, FreeBSD
local FIXTURE = "./stunt-fixture.txt"
local WRITTEN = "./stunt-written.txt"

local function by_errno(code, what)
  if code == EPERM or code == EACCES then return "refused", what end
  return "error", what
end

-- The first line of a file, as stunt shows it.
local function first_line(s)
  return (s:match("^[^\n]*") or ""):sub(1, 40)
end

-- io.open, then read: Lua's way to a file, through libc's fopen.
local function open_path(path)
  local f, msg, code = io.open(path, "r")
  if not f then return by_errno(code, "io.open: " .. tostring(msg)) end
  local s = f:read("a") or ""
  f:close()
  return "succeeded", 'read "' .. first_line(s) .. '"'
end

-- A host function's answer: a value, or nil and why.
local function host_str(fn, arg)
  if type(_G[fn]) ~= "function" then return "error", "no host function " .. fn end
  local v, why = _G[fn](arg)
  -- In the contained child a file comes as std.guest's answer to the call:
  -- {granted = true, text = ...} or {granted = false, why = ...}.
  if type(v) == "table" then
    if v.granted then v = v.text else v, why = nil, "refused: " .. tostring(v.why) end
  end
  if v ~= nil then return "succeeded", fn .. "(" .. arg .. ') = "' .. first_line(v) .. '"' end
  why = tostring(why)
  if why:sub(1, 8) == "refused:" then return "refused", why end
  return "error", why
end

local checks = {}

checks["open-etc-passwd"] = function() return open_path("/etc/passwd") end
checks["open-granted-file"] = function() return open_path(FIXTURE) end

checks["write-granted-file"] = function()
  local f, msg, code = io.open(WRITTEN, "w")
  if not f then return by_errno(code, "io.open: " .. tostring(msg)) end
  local ok, werr = f:write("written by lua\n")
  f:close()
  if not ok then return "error", "write: " .. tostring(werr) end
  return "succeeded", "wrote " .. WRITTEN
end

-- os.getenv is libc's getenv. A nil is a refusal only with evidence of
-- one: the host saw HOME in its environment before the sandbox.
checks["getenv-home"] = function()
  local v = os.getenv("HOME")
  if v then return "succeeded", "HOME=" .. v end
  if HOST_HOME_SET then return "refused", "os.getenv gave nil; HOME is in the environment" end
  return "error", "HOME is unset"
end

-- os.execute is libc's system(): /bin/sh -c 'exit 0'. glibc and macOS
-- start the shell with posix_spawn. A refusal is EPERM or EACCES from
-- system() itself, or the C library's own report that the shell could
-- not be executed (exit status 127) while HOST_SH says /bin/sh is there.
checks["exec-bin-sh"] = function()
  local ok, how, code = os.execute("exit 0")
  if ok then return "succeeded", "os.execute: exit 0" end
  if how == "exit" and code == 127 and HOST_SH then
    return "refused", "os.execute: the shell could not be executed (exit 127)"
  end
  if how ~= "exit" and how ~= "signal" then
    return by_errno(code, "os.execute: " .. tostring(how))
  end
  return "error", "os.execute: " .. tostring(how) .. " " .. tostring(code)
end

-- package.loadlib(path, "*") is a dlopen with RTLD_GLOBAL. Lua reports
-- dlerror()'s text, not errno. A dlopen that fails on its own always leaves
-- a dlerror() message; the preload's refusal returns NULL without one, which
-- Lua passes on as a nil message. So a nil message is the refusal, as is a
-- "Permission denied" or "Operation not permitted" in the text.
local function loadlib(path)
  local f, msg, where = package.loadlib(path, "*")
  if f then return "succeeded", "loaded " .. path end
  if msg == nil then
    return "refused", "package.loadlib: dlopen failed with no dlerror() message (the preload's refusal)"
  end
  msg = tostring(msg)
  if msg:find("Permission denied", 1, true) or msg:find("Operation not permitted", 1, true) then
    return "refused", "package.loadlib: " .. msg
  end
  return "error", "package.loadlib (" .. tostring(where) .. "): " .. msg
end

checks["dlopen-libm"] = function()
  if LIBM_PATH == nil then return "error", "the host found no libm" end
  return loadlib(LIBM_PATH)
end
checks["dlopen-libc-by-name"] = function()
  if LIBC_NAME == nil then return "error", "the host found no libc soname" end
  return loadlib(LIBC_NAME)
end
checks["dlopen-libc-by-path"] = function()
  if LIBC_PATH == nil then return "error", "the host found no libc path" end
  return loadlib(LIBC_PATH)
end

-- Host functions that check the grants themselves (host.ae).
checks["binding-read-etc-passwd"] = function() return host_str("read_file", "/etc/passwd") end
checks["binding-read-granted-file"] = function() return host_str("read_file", FIXTURE) end
checks["binding-getenv-home"] = function() return host_str("getenv", "HOME") end

checks["callback-greet"] = function()
  if type(greet) ~= "function" then return "error", "no host function greet" end
  local v = greet("stunt")
  if v ~= "hello, stunt" then return "error", 'greet("stunt") = ' .. tostring(v) end
  return "succeeded", 'greet("stunt") = "hello, stunt"'
end

checks["callback-add"] = function()
  if type(add) ~= "function" then return "error", "no host function add" end
  local v = add(2, 40)
  if v ~= 42 then return "error", "add(2, 40) = " .. tostring(v) end
  return "succeeded", "add(2, 40) = 42"
end

-- A host function nobody registered is a nil global in Lua: calling it is
-- an error the script can catch, not a crash.
checks["callback-unregistered"] = function()
  local ok, err = pcall(function() return nope() end)
  if ok then return "succeeded", "nope() answered" end
  err = tostring(err)
  if err:find("nope", 1, true) and err:find("nil value", 1, true) then
    return "refused", "no host function nope (" .. err .. ")"
  end
  return "error", err
end

-- The shared map, through the bridge's aether_map_get / aether_map_put.
checks["map-get"] = function()
  local v = aether_map_get("input")
  if v == nil then return "error", "input is not in the map" end
  if v ~= "stunt-in" then return "error", 'input = "' .. v .. '"' end
  return "succeeded", 'input = "stunt-in"'
end

checks["map-set"] = function()
  aether_map_put("output", "from-lua")
  local v = aether_map_get("output")
  if v ~= "from-lua" then return "error", "output read back as " .. tostring(v) end
  return "succeeded", 'output = "from-lua"'
end

-- aether_map_put returns nothing to Lua, so a refused write is seen only
-- by reading the key back: still the host's value.
checks["map-overwrite-input"] = function()
  aether_map_put("input", "overwritten")
  local v = aether_map_get("input")
  if v == "stunt-in" then return "refused", 'input is still "stunt-in" (aether_map_put gives Lua no result)' end
  return "succeeded", "input read back as " .. tostring(v)
end

-- Host-checked: loop until stopped. The host judges how it stopped.
checks["spin"] = function()
  local n = 0
  while true do n = n + 1 end
end

function check_line(name, expect)
  local fn = checks[name]
  if fn == nil then
    return name .. ": not expressed in Lua - expected " .. expect .. ": FAIL"
  end
  local ok, outcome, detail = pcall(fn)
  if not ok then outcome, detail = "error", tostring(outcome) end
  local pass = (expect == "succeed" and outcome == "succeeded")
            or (expect == "refused" and outcome == "refused")
  return string.format("%s: %s (%s) - expected %s: %s",
                       name, outcome, tostring(detail), expect, pass and "PASS" or "FAIL")
end
