# virtio-gpu

Tunix drives QEMU's virtio-gpu as a 2D display. Where the framebuffer DRM
device copies a client's buffer into the scanout every frame, virtio-gpu scans
that buffer out where it lies.

```sh
make run-gpu
```

## Why it exists

`/dev/dri/card0` has no GPU behind it (see `kernel/drivers/drm.c`). The display is
the region the bootloader set up over VBE, there is no CRTC to reprogram, and so
presenting a framebuffer means blitting it: a full screen of `memcpy` on the
CPU, per frame, in the kernel, with the giant lock held. At 1280x720 that is
3.5 MiB a frame.

A virtio-gpu host resource can be backed by the guest pages that already hold
the buffer. Presenting then becomes three commands on a queue — point the
scanout at the resource, tell the host the guest pages changed, flush — and the
screenful of copying goes away.

## The pieces

| File | What it is |
| --- | --- |
| `kernel/drivers/virtio/virtio_pci.c` | The virtio 1.0 PCI transport |
| `kernel/drivers/virtio/virtio_ring.c` | A split virtqueue |
| `kernel/drivers/virtio/virtio_gpu.c` | The device: resources, scanout, flush |
| `kernel/include/virtio.h`, `virtgpu.h` | The interfaces between them |

A modern virtio device publishes no registers at a fixed offset. It chains
vendor-specific PCI capabilities, each naming a BAR, an offset and a length, and
the driver walks that chain to find the common configuration, the notification
area and the device configuration. `virtio_pci.c` maps whichever BARs those name
into the shared device window described in `kernel/include/vmm.h`.

The transport is **polled**. After a kick it spins on the used ring with a two
second deadline, the same bargain the RTL8139 driver makes, and for the same
reason: there is one request outstanding at a time and nothing to gain from an
interrupt. One request also means the descriptor table needs no allocator — a
chain is always taken from the head.

## What presenting looks like

A dumb buffer gets a host resource the first time it reaches the screen, backed
by its own pages through `RESOURCE_ATTACH_BACKING`, one memory entry per page.
The resource stays on the buffer, so the cost is paid once. `SET_SCANOUT` is
issued only when the buffer being presented is not the one already scanned out,
which is every other frame under a compositor that double-buffers.

Formats: `DRM_FORMAT_XRGB8888` is `VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM`. Those name
the same four bytes; DRM names them as a little-endian word and virtio names
them in memory order.

The resource is created `pitch / 4` pixels wide rather than the framebuffer's
width, because a 2D transfer reads the guest pages at the resource's stride and
those two are only the same number when the buffer has no padding.

## Giving the display back

The text console draws into the VGA framebuffer, and the first answer to
handing the display back was to set the scanout to nothing, on the grounds that
the device would then show what was underneath.

**It does not.** A `virtio-vga` shows the VGA framebuffer only until the guest
sets a scanout for the first time; from then on the display is the virtio one,
and a scanout set to nothing reads *Display output is not active*. Switching
away from a compositor left a blank screen where the console should have been.

So the console is scanned out like anything else. `drm_console_present()`
creates one resource over the framebuffer the bootloader set up — the pages
never move, so it is created once — and points the scanout at it. Because a
host resource is a copy of the guest pages rather than a window onto them, it
has to be transferred again as the console changes: the timer does that thirty
times a second, and only while a text console is what the user is looking at.
A screenful of transfer at that rate is what the display cost before virtio-gpu
existed, and it is paid only while nothing else wants the screen.

`run-gpu` uses `virtio-vga` rather than `virtio-gpu-pci` because the
VGA-compatible variant is the one that has a framebuffer for the bootloader to
set a mode in and for the console to draw into.

## Resolution

Three things name a size and they have to be the same one:

- the mode the bootloader sets over VBE, which becomes the kernel framebuffer
  and, through `MODE_GETCONNECTOR`, the only mode DRM reports
- the rect a scanout is set to, which is the framebuffer's
- whatever the device answers `GET_DISPLAY_INFO` with

Only the first two matter. The third is **not a mode**: QEMU answers with the
`xres`/`yres` properties until a window manager tells it how big the window is,
and with the window from then on. So it moves when the window is dragged, and on
a fresh `virtio-vga` it starts at the VGA adapter's 640x480 — nothing to do with
what the bootloader set. Nothing here scans out at it, and the driver no longer
prints it, because a boot log that reports the host's window as "the display" is
worth an afternoon of chasing the wrong thing.

`run-gpu` still passes `xres=1280,yres=720` (`QEMU_GPU_RESOLUTION`), which keeps
that number coherent when there is no window to override it — a headless run.

**A small window is a host window, not a small desktop.** QEMU opens it at
640x480 and does not reliably grow it when the bootloader sets 1280x720, so the
desktop ends up letterboxed. `run-gpu` passes `-display gtk,zoom-to-fit=on`
(`QEMU_GPU_DISPLAY`) so the guest fills whatever the window is and maximising it
gives a full-size desktop; `QEMU_GPU_DISPLAY=gtk,full-screen=on` skips the step.
The guest is unaffected either way — DRM reports one mode, the bootloader's, and
nothing reads the host's ui_info.

## 3D

The driver negotiates `VIRTIO_GPU_F_VIRGL` where the host offers it, so mesa
finds a real driver on `/dev/dri/renderD128` and the host renders on its own
GPU. `make run-virgl` is the target that asks QEMU for it; without it the device
is the same one in 2D mode and mesa rasterises with llvmpipe.

The difference is the whole point: SuperTuxKart's own profiling lap runs at 49
frames a second on llvmpipe and 110 through virgl.

## Where a frame's time goes

Asked, before anything was changed, with counters around each part of a
submission while the game ran. Per second of wall clock:

```
executions      431/s        command bytes   1755 KiB/s
copying them    1 ms         staging them    1 ms
allocating      31 ms        waiting for the host   609 ms
```

So the copies are nothing, and the driver spent between 60 and 73 per cent of
every second inside a spin waiting for the host to finish a command, with the
kernel lock held, so no other processor could run kernel code either. There
were about seventeen of those round trips per frame.

A request that carries no answer does not need waiting for. Submissions now
carry their own request, payload and response memory out of a pool of 32 slots
and are posted and left; `virtio_queue_post` allocates descriptors from a free
list, `virtio_queue_reclaim` takes them back, and only a caller that reads a
response drains the queue. Measured on the same image and the same client, the
one difference being whether the path was compiled in:

```
waiting for the host   206 ms/s   ->   20 ms/s
```

**It did not make the game faster**, and that is the finding: 116 frames a
second before, 115 after. The waiting *was* the host rendering, so removing it
gives the guest its processor back rather than more frames. What is faster is
everything else on the machine, which is no longer queued behind a driver that
holds the kernel lock for two thirds of every second.

Two things make posting safe. Ordering: this queue is answered in order, so a
command that reads back data, and any caller that waits, sees everything posted
before it. And `DRM_IOCTL_VIRTGPU_WAIT`, which used to answer yes without
looking and now drains the queue: mesa marks a resource busy when a submission
mentions it and asks here before touching it again.

## The crash that was blamed on mesa

SuperTuxKart fell over at track load, at a null dereference inside
`dri2_query_image`, often enough to be called intermittent and long enough to
be assumed somebody else's bug. It was this driver's table of buffer handles:
512 entries, a number with no reason behind it, and a track's textures, vertex
buffers and render targets run past it. mesa answers a refused resource with a
null image and reads through it.

The table is 4096 entries now and a handle is one more than the slot it names,
so finding one is a subtraction rather than a walk. The lap that had been
failing about half the time has not failed since.

## What is deliberately not here

**A queue interrupt that anything waits on.** The device has a vector, bound
through MSI-X, and the driver asks it not to use it: the available ring carries
`VIRTQ_AVAIL_F_NO_INTERRUPT` while a request is being waited out. Sleeping
instead of spinning would mean giving back a processor that is holding the
kernel lock, so every other processor would stop too and the machine would wait
exactly as long, having also stopped. It was tried, and it cost SuperTuxKart its
whole start-up. The wait becomes a sleep when that lock is no longer the whole
kernel's.

**A fence.** Waiting for one resource waits for every submission, because
nothing here records which submission touched what. It is never wrong, only
sometimes more than was asked for.

**A second scanout.** DRM reports one CRTC and one connector, so there is
nothing above this that could ask for one.

## Verified

On `make run-gpu`, against QEMU 11.0.2 with `virtio-vga`:

- the device probes at `1af4:1050`, negotiates `VERSION_1` only, and
  `GET_DISPLAY_INFO` round-trips — `TUNIX: virtio-gpu ready` on the serial log
- the console renders through the scanout, confirmed with the blit fallback
  compiled out so that nothing could be reaching the screen by the old path
- the fallback itself still works: a machine with a plain VGA adapter finds no
  virtio-gpu and `drm.c` blits exactly as before
