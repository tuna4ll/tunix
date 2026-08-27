# Users and Permissions

Tunix used to have exactly one identity. Every process was root, `getuid()`
answered 0 because there was nothing else it could answer, and the only thing a
path check asked was whether the file existed. This describes what replaced it.

## Accounts

The image ships two people and the usual system accounts:

| Account | uid | Password | Home |
| --- | --- | --- | --- |
| `root` | 0 | `root` | `/root` |
| `tunix` | 1000 | `tunix` | `/home/tunix` |

`tunix` is in `wheel` (so `sudo` works), and in `audio`, `video` and `input`,
which is how an unprivileged desktop reaches the sound card, the display and the
keyboard. The groups are not decoration: the device nodes carry them.

| Device | Group |
| --- | --- |
| `/dev/dri/card0`, `/dev/fb0` | `video` (44) |
| `/dev/input/event*` | `input` (45) |
| `/dev/snd/*` | `audio` (29) |
| `/dev/pts/*` | `tty` (5) |
| `/dev/sda`, `/dev/sda1`, ... | `disk` (6) |

## Logging in

runit runs an `agetty` on each of the first four virtual terminals; `login(1)`
from shadow-utils checks `/etc/shadow`, sets the account's groups, gid and uid,
and executes the login shell. Everything after that runs as whoever logged in.

Both accounts share one password hash, and it is in
`base-files/append/shadow` in plain sight: this is a machine you boot in an
emulator to look at, not one anybody logs into over a network.

## Becoming somebody else

`su`, `sudo` and `passwd` are Void's own binaries, unmodified. They work because
the kernel honours the set-user-ID bit: they are installed 4755, so an exec of
them runs with euid 0 while the real uid stays the caller's, and dropping back
is what the saved uid is for.

```
$ id
uid=1000(tunix) gid=1000(tunix) groups=1000(tunix),4(wheel)
$ sudo id -u
[sudo] password for tunix:
0
$ su -
Password:
# whoami
root
```

`base-files/overlay/etc/sudoers.d/tunix` is what gives the account access.

## What the kernel enforces

Credentials live on the process: real, effective and saved uid/gid, the
filesystem uid/gid, and up to 32 supplementary groups. They are inherited across
`fork()` and `clone()`, and an `execve()` of a setuid binary is the only way to
gain one you did not have.

Checked against the mode bits, with root overriding everything except execute
permission on a file that has no execute bit at all:

- `open()` -- read/write on the file, plus write and search on the directory
  when creating.
- `execve()` -- execute on the program and search on every directory leading to
  it. A script's interpreter is checked too; scripts never get setuid.
- `mkdir`, `rmdir`, `unlink`, `rename`, `symlink` -- write and search on the
  parent, and the sticky-bit rule in `/tmp`: only the owner may remove.
- `chdir` -- search.
- `chmod` -- owner or root. `chown` -- root, except that an owner may hand a
  file to one of their own groups; either way the setuid bits come off.
- `access()`/`faccessat()` -- the real ids, or the effective ones with
  `AT_EACCESS`.
- `kill()` -- the target must share the sender's identity, or the sender is root.

A new file is owned by whoever created it, with the group taken from the parent
directory when that directory is setgid.

The console has a session now, rather than a single global foreground process
group: `TIOCSCTTY` claims it, `TIOCNOTTY` gives it up, and the job-control rule
that stops a background process from reading the terminal only applies inside
the session that holds it. Without that, `login` was refused its own first read.
`/proc/self` and `/proc/<pid>/fd` exist for the same practical reason: that is
how `ttyname(3)` answers, and `su` will not run for a non-root caller whose
terminal it cannot name.

## How the image gets its permissions

From the sysroot, unchanged: `mkfs.ext2 -d` copies each file's mode and owner
into the filesystem it builds. There used to be a table of them applied to the
finished archive, because the tree was staged on a Windows drive that reports
everything as 0777 root:root -- `support/sysroot.sh` now refuses to build there
instead, and says where to put the sysroot rather than working around it. A
filesystem that cannot hold a setuid bit cannot hold a root filesystem.
