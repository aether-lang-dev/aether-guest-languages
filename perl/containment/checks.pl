# The rows of stunt's catalogue (aether contrib/host/stunt), each done the
# way a Perl script would do it, and the judge that turns what happened
# into stunt's one line:
#
#   <check>: <outcome> (<detail>) - expected <expectation>: PASS|FAIL
#
# The host (host.ae, or child_host.ae for the contained child) prepends a few
# facts it found before the sandbox was in force ($HOST_HOME_SET, $HOST_SH,
# $LIBM_PATH, $LIBC_NAME, $LIBC_PATH), appends a call of check_line(name,
# expectation) and reads the line back. A refusal is what a sandbox gives:
# EPERM or EACCES from libc, the preload library or the kernel, or a host
# function's error that starts "refused:". Anything else that goes wrong is
# an error, never a refusal, so it cannot pass for a sandbox at work.
#
# The modules used here are loaded by the host before the sandbox is in
# force (host.ae), or by the worker before it locks itself down
# (perl/worker): Perl reads a module from disk the first time it is loaded,
# so under a policy with no read grant on its library directories a module
# not yet loaded cannot be (perl/README.md). That is why `use` appears only
# for what is already loaded, and everything else is called by its full name.
#
# Rows Perl cannot express are not here: perl/containment/lang.sh says which
# and why.

use Errno ();
use Socket ();
use Fcntl ();
use POSIX ();
use DynaLoader ();

our ($HOST_HOME_SET, $HOST_SH, $LIBM_PATH, $LIBC_NAME, $LIBC_PATH);

our $FIXTURE = "./stunt-fixture.txt";
our $WRITTEN = "./stunt-written.txt";

sub by_errno {
    my ($code, $what) = @_;
    # EPERM and EACCES: the same numbers on Linux, macOS and FreeBSD.
    return ("refused", $what) if $code == 1 || $code == 13;
    return ("error", $what);
}

sub first_line {
    my ($s) = @_;
    $s = (split /\n/, $s, 2)[0];
    $s = '' unless defined $s;
    return substr($s, 0, 40);
}

# open(), then read: Perl's way to a file, through libc's open.
sub open_path {
    my ($path) = @_;
    my $fh;
    unless (open($fh, '<', $path)) {
        return by_errno($! + 0, "open: $!");
    }
    my $s = <$fh>;
    close($fh);
    $s = '' unless defined $s;
    return ("succeeded", 'read "' . first_line($s) . '"');
}

sub write_file {
    my $fh;
    unless (open($fh, '>', $WRITTEN)) {
        return by_errno($! + 0, "open: $!");
    }
    print $fh "written by perl\n";
    close($fh);
    return ("succeeded", "wrote $WRITTEN");
}

# %ENV: the copy of the environment Perl took when it started, before any
# sandbox. Core Perl has no way to ask libc's getenv (getenv-home is left
# out, lang.sh). A missing HOME is a refusal only when the host had one: then
# it was kept from this process (the bridge hides what the grants do not
# grant, and the contained child is given no environment).
sub env_cached {
    my $v = $ENV{HOME};
    return ("succeeded", "HOME=$v from %ENV") if defined $v;
    return ("refused", "HOME is not in %ENV; the host has it") if $HOST_HOME_SET;
    return ("error", "HOME was never in the environment");
}

# A non-blocking connect, judged as stunt judges it: EACCES at connect
# (the preload, Landlock) or EPERM/EACCES at socket (seccomp) is a refusal;
# EPERM at connect is a firewall's answer, an error; anything else
# (EINPROGRESS, a network error) means the call was let through.
sub connect_to {
    my ($family, $addr) = @_;
    my $s;
    unless (socket($s, $family, Socket::SOCK_STREAM(), 0)) {
        return by_errno($! + 0, "socket: $!");
    }
    my $flags = fcntl($s, Fcntl::F_GETFL(), 0);
    fcntl($s, Fcntl::F_SETFL(), $flags | Fcntl::O_NONBLOCK());
    my $sin;
    if ($family == Socket::AF_INET()) {
        $sin = Socket::pack_sockaddr_in(443, Socket::inet_aton($addr));
    } else {
        $sin = Socket::pack_sockaddr_in6(443, Socket::inet_pton($family, $addr));
    }
    my $ok = connect($s, $sin);
    my $e = $! + 0;
    close($s);
    return ("succeeded", "connected") if $ok;
    return ("refused", "connect: " . _strerror($e)) if $e == 13;
    return ("error", "connect: " . _strerror($e) . " (a firewall's answer, not the sandbox's)") if $e == 1;
    return ("succeeded", "let through: " . _strerror($e));
}

sub _strerror {
    my ($e) = @_;
    local $! = $e;
    return "$!";
}

# Starting /bin/sh, both ways Perl has: system (fork, then exec) and
# backticks (a pipe from a forked shell). Succeeded when either started a
# shell: an escape by one way is an escape. Refused when both were refused:
# the fork or the exec failed with EPERM or EACCES, or the shell could not be
# executed (exit 127) while $HOST_SH says /bin/sh is there.
sub exec_sh {
    my @ways;
    {
        local $! = 0;
        my $st = system("/bin/sh", "-c", "exit 0");
        my $e = $! + 0;
        if ($st == 0) {
            push @ways, ["succeeded", "system: exit 0"];
        } elsif ($st == -1) {
            push @ways, [by_errno($e, "system: " . _strerror($e))];
        } elsif (($st >> 8) == 127 && $HOST_SH) {
            push @ways, ["refused", "system: the shell could not be executed (exit 127)"];
        } else {
            push @ways, ["error", "system: status $st"];
        }
    }
    {
        local $! = 0;
        my $out = `exit 0; `;
        my $st = $?;
        my $e = $! + 0;
        if (defined $out && $st == 0) {
            push @ways, ["succeeded", "backticks: exit 0"];
        } elsif ($st == -1) {
            push @ways, [by_errno($e, "backticks: " . _strerror($e))];
        } elsif (($st >> 8) == 127 && $HOST_SH) {
            push @ways, ["refused", "backticks: the shell could not be executed (exit 127)"];
        } else {
            push @ways, ["error", "backticks: status $st"];
        }
    }
    my $detail = join("; ", map { $_->[1] } @ways);
    my @kinds = map { $_->[0] } @ways;
    return ("succeeded", $detail) if grep { $_ eq "succeeded" } @kinds;
    return ("refused", $detail) if "@kinds" eq "refused refused";
    return ("error", $detail);
}

# fork, the child exiting at once: 0 succeeded, anything else an error.
sub fork_once {
    my $pid = fork();
    unless (defined $pid) {
        return by_errno($! + 0, "fork: $!");
    }
    POSIX::_exit(0) if $pid == 0;
    waitpid($pid, 0);
    return ("succeeded", "the forked child ran") if $? == 0;
    return ("error", "the forked child: status $?");
}

# DynaLoader::dl_load_file is a dlopen. A dlopen that fails on its own leaves
# a dlerror() message; the preload's refusal returns NULL without one, so
# dl_error() is empty or "(null) at ... line N." (DynaLoader formats the NULL
# dlerror() returns into a message with perl's location on the end), and
# "Permission denied" or "Operation not permitted" in it is the refusal too.
sub load {
    my ($path) = @_;
    my $h = DynaLoader::dl_load_file($path, 0);
    return ("succeeded", "loaded $path") if $h;
    my $msg = DynaLoader::dl_error();
    $msg = '' unless defined $msg;
    $msg =~ s/\0.*//s;   # DynaLoader's message carries a trailing NUL
    if ($msg eq '' || $msg =~ /^\(null\)/ || $msg =~ /Permission denied|Operation not permitted/) {
        $msg =~ s/\n\z//;
        return ("refused", "dl_load_file: " . ($msg eq '' ? "no dlerror() message" : $msg) . " (the preload's refusal)");
    }
    return ("error", "dl_load_file: $msg");
}

sub need {
    my ($name) = @_;
    no strict 'refs';
    my $v = ${"main::$name"};
    die "the host found no $name\n" unless defined $v;
    return $v;
}

# libc's syscall(), Perl's own: what stunt calls syscall-fn-openat. Only EPERM
# or EACCES is a refusal (ENOSYS is the preload's syscall() with no sandbox
# in force, which is an error).
our %SYS_OPENAT = (x86_64 => 257, aarch64 => 56);
sub syscall_fn_openat {
    my $machine = (POSIX::uname())[4];
    my $nr = $SYS_OPENAT{$machine};
    return ("error", "no syscall numbers for $machine") unless defined $nr;
    my $path = "/etc/passwd";
    my $r = syscall($nr, -100, $path, Fcntl::O_RDONLY(), 0);
    if ($r >= 0) {
        POSIX::close($r);
        return ("succeeded", "opened /etc/passwd");
    }
    return by_errno($! + 0, "syscall(): " . _strerror($! + 0));
}

# A host function's answer. In-process (host.ae) a refusal is the die the host
# raised ("refused: ..."); in the contained child a file comes as std.guest's
# answer to the call, {"granted": true, "text": ...} or {"granted": false,
# "why": ...}, and a FAIL is a die too.
sub host_str {
    my ($fn, $arg) = @_;
    no strict 'refs';
    return ("error", "no host function $fn") unless defined &{"main::$fn"};
    my $v = eval { &{"main::$fn"}($arg) };
    if ($@ ne '') {
        my $why = $@;
        $why =~ s/\n\z//;
        return ("refused", $why) if $why =~ /^refused:/;
        return ("error", $why);
    }
    if (ref($v) eq 'HASH') {
        return ("refused", "refused: " . (defined $v->{why} ? $v->{why} : '')) unless $v->{granted};
        $v = $v->{text};
    }
    return ("error", "$fn answered undef") unless defined $v;
    return ("succeeded", "$fn($arg) = \"" . first_line($v) . "\"");
}

sub callback_greet {
    my $v = greet("stunt");
    return ("error", "greet(\"stunt\") = " . (defined $v ? "\"$v\"" : "undef")) unless defined $v && $v eq "hello, stunt";
    return ("succeeded", 'greet("stunt") = "hello, stunt"');
}

sub callback_add {
    my $v = add(2, 40);
    return ("error", "add(2, 40) = " . (defined $v ? $v : "undef")) unless defined $v && $v == 42;
    return ("succeeded", "add(2, 40) = 42");
}

# A host function nobody registered is an undefined subroutine in Perl:
# calling it is a die the script can catch, not a crash.
sub callback_unregistered {
    my $r = eval { nope(); 1 };
    if (!$r && $@ =~ /^Undefined subroutine/) {
        my $m = $@;
        $m =~ s/ at .*//s;
        return ("refused", "no host function nope ($m)");
    }
    return ("succeeded", "nope() answered");
}

# The shared map, through the bridge's aether_map_get / aether_map_put (the
# host's own map in the child).
sub map_get {
    my $v = aether_map_get("input");
    return ("error", "input is not in the map") unless defined $v;
    return ("error", "input = \"$v\"") unless $v eq "stunt-in";
    return ("succeeded", 'input = "stunt-in"');
}

sub map_set {
    aether_map_put("output", "from-perl");
    my $v = aether_map_get("output");
    return ("error", "output read back as " . (defined $v ? "\"$v\"" : "undef")) unless defined $v && $v eq "from-perl";
    return ("succeeded", 'output = "from-perl"');
}

# aether_map_put answers whether it wrote, but the child's host answers
# true or false through a call, so the row reads the key back, as every
# language's does: still the host's.
sub map_overwrite_input {
    aether_map_put("input", "overwritten");
    my $v = aether_map_get("input");
    return ("refused", 'input is still "stunt-in" (read back)') if defined $v && $v eq "stunt-in";
    return ("succeeded", "input read back as " . (defined $v ? "\"$v\"" : "undef"));
}

# half-frame is judged by the host (child only): see child_host.ae. What the
# script does is in the host's script, not here.

# Not stunt's: a module loaded under the sandbox (tests in
# perl/containment/test_imports.sh). Perl reads a module from disk the first
# time it is loaded, so under a policy with no read grant on its library
# directories the require is refused ("Permission denied", from the open of
# its source); one already in %INC is not read again.
sub import_module {
    my ($mod) = @_;
    (my $file = "$mod.pm") =~ s{::}{/}g;
    my $already = exists $INC{$file};
    my $ok = eval { require $file; 1 };
    return ("succeeded", "require $mod" . ($already ? " (already loaded)" : "")) if $ok;
    my $err = $@;
    $err =~ s/\n.*//s;
    return ("refused", "require $mod: $err") if $err =~ /Permission denied/;
    return ("error", "require $mod: $err");
}

our %CHECKS = (
    "open-etc-passwd" => sub { open_path("/etc/passwd") },
    "open-granted-file" => sub { open_path($FIXTURE) },
    "write-granted-file" => \&write_file,
    "env-cached" => \&env_cached,
    "connect-192.0.2.1" => sub { connect_to(Socket::AF_INET(), "192.0.2.1") },
    "connect-ipv4-mapped" => sub { connect_to(Socket::AF_INET6(), "::ffff:192.0.2.1") },
    "exec-bin-sh" => \&exec_sh,
    "fork" => \&fork_once,
    "dlopen-libm" => sub { load(need("LIBM_PATH")) },
    "dlopen-libc-by-name" => sub { load(need("LIBC_NAME")) },
    "dlopen-libc-by-path" => sub { load(need("LIBC_PATH")) },
    "syscall-fn-openat" => \&syscall_fn_openat,
    "binding-read-etc-passwd" => sub { host_str("read_file", "/etc/passwd") },
    "binding-read-granted-file" => sub { host_str("read_file", $FIXTURE) },
    "binding-getenv-home" => sub { host_str("getenv", "HOME") },
    "callback-greet" => \&callback_greet,
    "callback-add" => \&callback_add,
    "callback-unregistered" => \&callback_unregistered,
    "map-get" => \&map_get,
    "map-set" => \&map_set,
    "map-overwrite-input" => \&map_overwrite_input,
);

sub check_line {
    my ($name, $expect) = @_;
    my $fn = $CHECKS{$name};
    if (!$fn && $name =~ /^import:(.*)$/) {
        my $mod = $1;
        $fn = sub { import_module($mod) };
    }
    return "$name: not expressed in Perl - expected $expect: FAIL" unless $fn;
    my ($outcome, $detail);
    my @r = eval { $fn->() };
    if ($@ ne '') {
        my $e = $@;
        $e =~ s/\n\z//;
        ($outcome, $detail) = ("error", "die: $e");
    } else {
        ($outcome, $detail) = @r;
    }
    # "error" is for a row the harness knows fails for a reason of its own
    # (tests/stunt_lib.sh's preload_fails), so the day it stops failing the
    # row goes red.
    my $passed = ($expect eq "succeed" && $outcome eq "succeeded")
        || ($expect eq "refused" && $outcome eq "refused")
        || ($expect eq "error" && $outcome eq "error");
    return "$name: $outcome ($detail) - expected $expect: " . ($passed ? "PASS" : "FAIL");
}
