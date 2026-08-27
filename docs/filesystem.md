# The root filesystem (ext2)

The root is a real ext2 filesystem on the second partition of the boot disk.
The kernel mounts it at boot and writes through to it: reads, `mmap` and `exec`
are served from RAM, every mutation reaches the disk as it happens, and what
you edit is still there next boot.

## Where it comes from

`mkfs.ext2 -d` on the build host, from the Void sysroot. That is a change: the
kernel used to make the filesystem itself, formatting a region of the disk on
first boot and seeding it from an initramfs. Both are gone -- there is no
initramfs, and there is no format code in the kernel.

Which means the driver no longer gets to assume the layout it would have
written. It used to check that every group's bitmaps and inode table were
exactly where its own format code would have put them, and refuse the disk
otherwise. mke2fs does none of that reliably: most groups have no superblock
backup (`sparse_super`), and reserved growth blocks push the rest along. So the
group descriptors are read and believed, and what is checked is only that each
one points inside the filesystem.

## What the driver can mount

`superblock_usable()` in `kernel/ext2.c` refuses anything it genuinely cannot
read:

- a block size other than 4 KiB -- every buffer in the file is sized to it
- an inode larger than the classic 128 bytes
- a group whose bitmap would not fit in one block
- any incompatible feature but `filetype` -- extents and 64-bit block numbers
  above all

Which is why `support/image.sh` makes the filesystem with an explicit feature
set rather than mke2fs's defaults:

```sh
mkfs.ext2 -b 4096 -I 128 \
    -O ^resize_inode,^dir_index,^ext_attr,^metadata_csum,^64bit,^huge_file,^dir_nlink,^extra_isize
```

`dir_index` is off because the driver reads linear directory entries and writing
into a hashed directory without maintaining the tree would corrupt it for Linux.
`metadata_csum` is off for the same reason: a write that does not update the
checksum makes e2fsck complain about a filesystem that is otherwise fine.

The image is a plain ext2 filesystem, so it mounts on Linux:

```sh
sudo mount -o loop,offset=$((133120*512)) build/tunix.img /mnt
```

## Volatile directories

`/tmp`, `/var/tmp`, `/run`, `/dev` and `/proc` are marked `VFS_VOLATILE` and
behave like tmpfs mounts: they exist every boot and nothing under them touches
the disk. So does a FIFO -- `mkfifo` makes an in-memory node, because a FIFO
carries nothing across a reboot and persisting one would mean teaching the
driver a file type whose contents do not exist.

The sysroot's own `/dev`, `/proc`, `/sys`, `/run` and `/tmp` are emptied before
the image is made. Anything left in them would sit underneath the kernel's own
and shadow it: an install script that redirected to `/dev/null` once left a
twenty-byte regular file there, and every `>/dev/null` in the system then failed
with `EACCES`.

## Partitions

`kernel/partition.c` reads GPT and MBR partition tables and registers what they
describe as block devices of their own -- `sda1`, `sda2` -- so the filesystem
starts where its device does and `root=/dev/sda2` names it. Both schemes are
read because both are used: the image carries a protective MBR in front of the
GPT so that one disk boots either firmware.

## What is not here

- **No journal.** ext2, not ext3.
- **No extents, no 64-bit block numbers.** 16 GiB is the ceiling.
- **No `dir_index`.** A directory is a linear scan.
- **`fsck` on the running root** is not something to do; the fstab entry has
  pass 0 for exactly that reason.
