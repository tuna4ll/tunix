# Mounting

Tunix has one filesystem tree. `mount(2)` and `umount2(2)` change what a path
in it reaches.

## What a mount is here

A directory can carry a filesystem mounted over it. `vfs_find_child()` steps
onto the mounted root whenever a path walk reaches such a directory — the same
place hard links are resolved — so nothing above the VFS has to know a mount
was crossed.

The mounted root takes the mountpoint's **name** and the mountpoint's
**parent**. That is what makes `..` leave the mount and `vfs_node_path()`
spell the path the caller actually walked, without a single special case in
either. The mountpoint node keeps its own children; they are simply
unreachable until the filesystem is unmounted, exactly as on Linux.

A tmpfs root is marked `VFS_VOLATILE`, which is what stops the ext2 driver
writing any of it to the disk. That flag, not the mount, is what makes it a
temporary filesystem.

## What can be mounted

| Type | Meaning |
|---|---|
| `tmpfs`, `ramfs` | a fresh, empty, RAM-only tree |
| `--bind` (`MS_BIND`) | show a directory that already exists at a second place |
| `proc`, `sysfs`, `devtmpfs` | already mounted where they belong; mounting one elsewhere is refused, since only their own drivers can build them |
| anything else | `ENODEV` |

`/proc/mounts` is the real table, root first:

```
/dev/sda / ext2 rw 0 0
tmpfs /tmp tmpfs rw 0 0
tmpfs /var/tmp tmpfs rw 0 0
tmpfs /run tmpfs rw 0 0
devtmpfs /dev devtmpfs rw 0 0
sysfs /sys sysfs rw 0 0
proc /proc proc rw 0 0
```

The trees the system boots with are in the table because that is what they are
to userspace, but they have no mountpoint to restore and `umount` refuses them
with `EPERM`.

## From the shell

`mount` and `umount` are first-party tools, not util-linux:

```sh
mount                                   # print /proc/mounts
mount -t tmpfs tmpfs /mnt/scratch
mount --bind /etc /mnt/etc
mount -o remount,ro /mnt/scratch
umount /mnt/scratch
```

An unknown `-o` name is an error rather than being ignored — silently dropping
`ro` would mount read-write.

## Checking it

`mount-test` on the image asserts what a mount is *for* rather than what it
returns: a file written at the mountpoint disappears when a filesystem is
mounted over it and comes back when it is unmounted, a file written inside the
mounted tree does not survive the unmount, and a bind mount makes `/etc/passwd`
readable through a second path while leaving the original alone.

## Limits

- **No block-device filesystem can be mounted.** `ext2.c` keeps its superblock,
  group descriptors, caches and persistence hooks in file-scope globals, and
  `vfs_persist_ops` has no per-mount context, so exactly one ext2 volume can
  exist — the root. Mounting a second disk means making that driver
  multi-instance first.
- Only one filesystem per mountpoint: mounting over an existing mount is
  refused with `EBUSY` rather than stacked, because two entries with the same
  target would leave `umount` unable to say which it meant.
- `MS_RDONLY`, `MS_NOSUID`, `MS_NODEV` and `MS_NOEXEC` are recorded and
  reported in `/proc/mounts`, but **nothing enforces them**.
- Mounts are global. There are no mount namespaces, so one process's mount is
  every process's mount, and only root may call `mount`.
- A bind mount resolves paths correctly, but `getcwd` inside one reports the
  directory's original path, since the VFS derives a path from the node rather
  than from the walk that reached it.
