# The rows of stunt's catalogue (aether contrib/host/stunt) that Tcl can
# express, each done the way a Tcl script would do it, and the judge that
# turns what happened into stunt's one line:
#
#   <check>: <outcome> (<detail>) - expected <expectation>: PASS|FAIL
#
# The host (host.ae, or child_host.ae for the contained child) prepends a few
# facts it found before the sandbox was in force (HOST_HOME_SET, HOST_SH,
# LIBM_PATH, LIBC_NAME, LIBC_PATH), appends a call of check_line name
# expectation and reads the line back. A refusal is what a sandbox gives:
# EPERM or EACCES from libc, the preload library or the kernel, or a host
# command's error that starts "refused:". Anything else that goes wrong is an
# error, never a refusal, so it cannot pass for a sandbox at work. The rows
# Tcl cannot express are left out by name in containment/lang.sh.
#
# Written for Tcl 8.5 and later: dict, lassign and `in`, no lmap, try or
# string cat.

set FIXTURE ./stunt-fixture.txt
set WRITTEN ./stunt-written.txt

namespace eval check {}

# An outcome from a failed command's -errorcode: POSIX EPERM or EACCES is a
# refusal, anything else an error.
proc classify {opts what} {
    set ec ""
    if {[dict exists $opts -errorcode]} { set ec [dict get $opts -errorcode] }
    if {[lindex $ec 0] eq "POSIX" && [lindex $ec 1] in {EPERM EACCES}} {
        return [list refused $what]
    }
    return [list error "$what (errorCode $ec)"]
}

# The first line of a string, as stunt shows it.
proc first_line {s} {
    return [string range [lindex [split $s "\n"] 0] 0 39]
}

# open, then read: Tcl's way to a file, through libc's open.
proc open_path {path} {
    if {[catch {open $path r} chan opts]} { return [classify $opts "open: $chan"] }
    set s [read $chan]
    close $chan
    return [list succeeded "read \"[first_line $s]\""]
}

proc check::open-etc-passwd {} { return [open_path /etc/passwd] }
proc check::open-granted-file {} { return [open_path $::FIXTURE] }

proc check::write-granted-file {} {
    if {[catch {open $::WRITTEN w} chan opts]} { return [classify $opts "open: $chan"] }
    puts $chan "written by tcl"
    close $chan
    return [list succeeded "wrote $::WRITTEN"]
}

# $env is the copy of the environment Tcl took when the interpreter was made
# (Tcl_Init, before any sandbox); reading it is an array read, which no libc
# interposer sees. Tcl never calls getenv, so getenv-home is left out.
proc check::env-cached {} {
    if {[info exists ::env(HOME)]} { return [list succeeded "HOME=$::env(HOME)"] }
    if {$::HOST_HOME_SET} {
        return [list refused "env(HOME) is not set; HOME is in the host's environment"]
    }
    return [list error "HOME is unset"]
}

# exec runs a program: fork, then execve in the child, both libc calls. A
# refusal is EPERM or EACCES from either ("couldn't fork child process" or
# "couldn't execute").
proc check::exec-bin-sh {} {
    if {[catch {exec /bin/sh -c "exit 0"} res opts]} { return [classify $opts "exec: $res"] }
    return [list succeeded "exec /bin/sh: exit 0"]
}

# socket -async: a non-blocking connect, which is what stunt asks for. The
# address is nobody's (TEST-NET-1), so a connect that is let through never
# completes; the channel is closed at once.
#
# A refusal here is EPERM or EACCES, or, for a socket the kernel will not
# make, EHOSTUNREACH: when every address Tcl tried failed in socket(), Tcl
# reports "host is unreachable" whatever errno was (strace on the contained
# child shows socket(AF_INET, SOCK_STREAM) = -1 EPERM, the seccomp filter
# with no tcp grant, and Tcl's error is EHOSTUNREACH). A connect that is let
# through and then finds no route fails later, in fconfigure -error, not here.
proc connect_to {host} {
    if {[catch {socket -async $host 443} chan opts]} {
        set ec [dict get $opts -errorcode]
        if {[lindex $ec 0] eq "POSIX" && [lindex $ec 1] eq "EHOSTUNREACH"} {
            return [list refused "socket: $chan (Tcl's report of a socket() that failed)"]
        }
        return [classify $opts "socket: $chan"]
    }
    catch {close $chan}
    return [list succeeded "socket -async $host 443: connect started"]
}

proc check::connect-192.0.2.1 {} { return [connect_to 192.0.2.1] }
# Over AF_INET6, which a grant for 192.0.2.1 must cover.
proc check::connect-ipv4-mapped {} { return [connect_to ::ffff:192.0.2.1] }

# load is a dlopen of the file, then a dlsym of <prefix>_Init. A library that
# dlopen finds but has no such function ends "couldn't find procedure" (8.5)
# or "cannot find symbol" (8.6): the load succeeded, as far as these rows go. A dlopen that fails on its own
# leaves a dlerror() message; the preload's refusal returns NULL without one,
# so the message ends where the dlerror text would begin.
proc loadlib {path} {
    if {![catch {load $path Aethergl} res opts]} { return [list succeeded "loaded $path"] }
    if {[string first "couldn't find procedure" $res] >= 0 ||
        [string first "cannot find symbol" $res] >= 0} {
        return [list succeeded "loaded $path (it has no Aethergl_Init, as expected)"]
    }
    if {[regexp {couldn't load (?:file|library) "[^"]*": *(\(null\))?$} $res] ||
        [string first "Permission denied" $res] >= 0 ||
        [string first "Operation not permitted" $res] >= 0} {
        return [list refused "load: $res"]
    }
    return [list error "load: $res"]
}

proc check::dlopen-libm {} {
    if {![info exists ::LIBM_PATH]} { return [list error "the host found no libm"] }
    return [loadlib $::LIBM_PATH]
}
proc check::dlopen-libc-by-name {} {
    if {![info exists ::LIBC_NAME]} { return [list error "the host found no libc soname"] }
    return [loadlib $::LIBC_NAME]
}
proc check::dlopen-libc-by-path {} {
    if {![info exists ::LIBC_PATH]} { return [list error "the host found no libc path"] }
    return [loadlib $::LIBC_PATH]
}

# A host command's answer: a value, or an error. In the contained child a
# granted file comes as std.guest's answer to the call, decoded to a dict:
# {granted 1 text ...} or {granted 0 why ...}.
proc host_str {fn arg} {
    if {[info commands $fn] eq ""} { return [list error "no host command $fn"] }
    if {[catch {$fn $arg} v]} {
        if {[string match "refused:*" $v]} { return [list refused $v] }
        return [list error $v]
    }
    if {![catch {dict get $v granted} g]} {
        if {$g} {
            set v [dict get $v text]
        } else {
            return [list refused "refused: [dict get $v why]"]
        }
    }
    return [list succeeded "${fn}($arg) = \"[first_line $v]\""]
}

proc check::binding-read-etc-passwd {} { return [host_str read_file /etc/passwd] }
proc check::binding-read-granted-file {} { return [host_str read_file $::FIXTURE] }
proc check::binding-getenv-home {} { return [host_str getenv HOME] }

proc check::callback-greet {} {
    if {[info commands greet] eq ""} { return [list error "no host command greet"] }
    set v [greet stunt]
    if {$v ne "hello, stunt"} { return [list error "greet stunt = $v"] }
    return [list succeeded {greet stunt = "hello, stunt"}]
}

proc check::callback-add {} {
    if {[info commands add] eq ""} { return [list error "no host command add"] }
    set v [add 2 40]
    if {$v ne "42"} { return [list error "add 2 40 = $v"] }
    return [list succeeded "add 2 40 = 42"]
}

# A command nobody registered is an error the script can catch, not a crash.
proc check::callback-unregistered {} {
    if {[catch {nope} err]} {
        if {[string first {invalid command name "nope"} $err] >= 0} {
            return [list refused "no host command nope ($err)"]
        }
        return [list error $err]
    }
    return [list succeeded "nope answered"]
}

# The shared map, through the bridge's aether_map_get / aether_map_put.
proc check::map-get {} {
    set v [aether_map_get input]
    if {$v eq ""} { return [list error "input is not in the map"] }
    if {$v ne "stunt-in"} { return [list error "input = \"$v\""] }
    return [list succeeded {input = "stunt-in"}]
}

proc check::map-set {} {
    aether_map_put output stunt-out
    set v [aether_map_get output]
    if {$v ne "stunt-out"} { return [list error "output read back as \"$v\""] }
    return [list succeeded {output = "stunt-out"}]
}

# aether_map_put gives Tcl no result, so a refused write is seen only by
# reading the key back: still the host's value.
proc check::map-overwrite-input {} {
    aether_map_put input overwritten
    set v [aether_map_get input]
    if {$v eq "stunt-in"} {
        return [list refused {input is still "stunt-in" (aether_map_put gives Tcl no result)}]
    }
    return [list succeeded "input read back as \"$v\""]
}

proc check_line {name expect} {
    if {[info commands check::$name] eq ""} {
        return "$name: not expressed in Tcl - expected $expect: FAIL"
    }
    if {[catch {check::$name} res]} { set res [list error $res] }
    lassign $res outcome detail
    set pass [expr {($expect eq "succeed" && $outcome eq "succeeded") ||
                    ($expect eq "refused" && $outcome eq "refused")}]
    return [format "%s: %s (%s) - expected %s: %s" $name $outcome $detail $expect \
                [expr {$pass ? "PASS" : "FAIL"}]]
}
