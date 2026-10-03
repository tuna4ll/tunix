# Limits

What used to be a fixed number in the kernel, what it is now, and how it is
tested. The test is `support/tests/limits-kerneltest.sh`; its sections are
selected with `CFLAGS_EXTRA=-DLIMITS_TESTS=<mask>` and the defaults run on
both architectures. Filesystems have their own, `support/tests/ext2-kerneltest.sh`.

## Removed

| Area | Was | Now |
| --- | --- | --- |
| Physical memory | 8 GiB (tracking reserve after the image) | the whole memory map, up to the 62.5 TiB direct map |
| Kernel heap block | 4 GiB (`uint32_t` size) | 64-bit sizes, segregated free lists |
| Paths | 256 bytes, names 128 | `PATH_MAX` 4096, `NAME_MAX` 255 |
| Directory depth | 64 levels (VFS and ext2 load) | none; traversals are iterative |
| Symlink chain | 16 | 40, like Linux |
| Processes | 256 address spaces | unbounded; pids, children, waiters and the run queue are indexed |
| File descriptors | 256 | `RLIMIT_NOFILE`, 1024 soft / 1048576 hard, with real `setrlimit` |
| `poll`/`select` | 256 / 1024 fds | any number up to the descriptor limit |
| `execve` | 512 arguments, 4 KiB strings, 256 KiB total | a quarter of `RLIMIT_STACK` (6 MiB cap), 128 KiB strings |
| User stack | 8 MiB | `RLIMIT_STACK` |
| `/proc/pid/cmdline` | 512 bytes | read from the process's own memory |
| Processors | 8 | 256, with full affinity masks |
| Framebuffer | 1920x1080, else panic | anything that fits the 240 MiB window |
| ext2 | 128 groups (~16 GiB), 32-bit sector math | any group count, 64-bit sectors |
| ext2 / ext3 volumes | the root only, 4 KiB blocks, 128-byte inodes, files to 4 GiB | any number mounted, 1–4 KiB blocks, any inode size, files to 2 TiB, `dir_index` and `ext_attr` |
| TCP/UDP sockets | 32 system-wide, backlog 16 | unbounded, backlog 4096, hashed lookups |
| Datagrams | 8 queued, 2 KiB each, 1472 bytes sent | receive buffer bytes, 65507 bytes, IPv4 fragmentation |
| TCP | 16 KiB buffers, 1 KiB segments, 64 KiB window | buffers to `SO_RCVBUF`, MSS option, window scaling |
| Loopback | 40 packets of 1500 bytes | 16 MiB of packets, MTU 65536 |
| Terminals | 8 PTYs, 8 VTs | PTYs on demand, 63 VTs |
| epoll | 128 fds per instance, 128 events per wait | unbounded, scanned round robin |
| inotify | 64 watches, 8 KiB queue | unbounded watches hashed by node, queue grows to 1 MiB |
| Unix sockets | backlog 8, 64 records, 64 `SCM_RIGHTS` | backlog 4096, 4096 records, 253 rights, cwd-relative paths |
| Pipes | 64 KiB fixed | `F_SETPIPE_SZ`, 1 MiB for users |
| Supplementary groups | 32 | 65536 |
| memfd / SysV shm / shared file maps | 256 MiB / 128 × 64 MiB / 256 MiB | sparse, paged in on fault, no caps |
| File mappings | every page read and mapped at `mmap`, private writable maps copied | mapped on first touch, private pages copied on first write |
| `/proc` files | 4 KiB, silently cut | grow as needed (`cpuinfo` on 256 CPUs, `net/tcp` with thousands of sockets) |
| Device interrupts | 16 vectors | 128 |
| Device register window | 9 MiB | 512 GiB |
| Block devices | 32, disks `sda`–`sdz`, partitions 1–9 | unbounded, `sdaa` onward, any partition number |
| AHCI | first controller, 8 ports | every controller, 32 ports |
| NVMe | first controller, namespace 1 | every controller and namespace |
| xHCI / EHCI | 4 hosts, 32 / 8 devices, 4 HID interfaces | unbounded hosts, 255 / 127 devices, 16 interfaces |
| USB disks | 4 | unbounded |
| DRM | 64 framebuffers, 32 blobs, 8 GL contexts | unbounded |
| sysfs, PCI bindings, FAT volumes, eventfs subscribers | 64, 24, 4, 64 | unbounded |
| Kernel lock | one lock around every system call and interrupt | a lock per subsystem ([multiprocessor.md](multiprocessor.md#locking)) |

## Kept on purpose

ABI and hardware values stay: `UIO_MAXIOV` 1024, `EPOLL_MAX_NESTING` 5, the
shebang line (256 bytes, as Linux), RT priority 99, `sun_path` 108, the xAPIC id
space for processors.

## Still there

- ext2 has no extents or 64-bit block numbers: 16 TiB per filesystem.
