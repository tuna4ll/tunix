# virtio-gpu

Tunix drives QEMU's virtio-gpu as a 2D display. Where the framebuffer DRM
device copies a client's buffer into the scanout every frame, virtio-gpu scans
that buffer out where it lies.

```sh
make run-gpu
```

## Why it exists

`/dev/dri/card0` has no GPU behind it (see `src/kernel/drm.c`). The display is
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
| `src/kernel/virtio/virtio_pci.c` | The virtio 1.0 PCI transport |
| `src/kernel/virtio/virtio_ring.c` | A split virtqueue |
| `src/kernel/virtio/virtio_gpu.c` | The device: resources, scanout, flush |
| `src/kernel/include/virtio.h`, `virtgpu.h` | The interfaces between them |

A modern virtio device publishes no registers at a fixed offset. It chains
vendor-specific PCI capabilities, each naming a BAR, an offset and a length, and
the driver walks that chain to find the common configuration, the notification
area and the device configuration. `virtio_pci.c` maps whichever BARs those name
into the shared device window described in `src/kernel/include/vmm.h`.

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

The text console draws into the VGA framebuffer, which QEMU shows only while no
virtio scanout is set. So every path in `drm.c` that hands the display back —
`DROP_MASTER`, `SETCRTC` with no framebuffer, and the last descriptor on the
card closing — disables the scanout first, and the console reappears where it
left off. This is why `run-gpu` uses `virtio-vga` and not `virtio-gpu-pci`: the
VGA-compatible variant is the one that has a framebuffer for the bootloader to
set a mode in and for the console to draw into.

## Resolution

Three things name a size and they have to be the same one:

- the mode the bootloader sets over VBE, which becomes the kernel framebuffer
  and, through `MODE_GETCONNECTOR`, the only mode DRM reports
- the rect a scanout is set to, which is the framebuffer's
- whatever the device answers `GET_DISPLAY_INFO` with

The third is the odd one out. Left alone, QEMU answers with the `xres`/`yres`
properties — 1280x800 by default — and once a window manager has told it how big
the window is, with the window instead. On a fresh `virtio-vga` that is the VGA
adapter's 640x480, which is why `run-gpu` pins `xres=1280,yres=720` to the mode
the bootloader actually sets. Override `QEMU_GPU_RESOLUTION` to move all of it.

Nothing in the guest resizes itself to follow the host window: the driver reads
the display size once, at probe, and reports it. A QEMU window smaller than the
guest mode is a host-side window, not a smaller desktop — QEMU scales the
scanout down into it. "View → Zoom To Fit", or resizing the window, is the fix
for that, and `-display gtk,zoom-to-fit=on` sets it from the start.

## What is deliberately not here

**3D.** Nothing negotiates `VIRTIO_GPU_F_VIRGL`, so the device comes up in 2D
mode and mesa keeps rasterising with llvmpipe. virgl would need the host to have
a working GL stack and a render node, which is a property of the machine Tunix is
being run on rather than of Tunix.

**Interrupts, and more than one request in flight.** Both follow from the
polling above.

**A second scanout.** DRM reports one CRTC and one connector, so there is
nothing above this that could ask for one.

## Verified

On `make run-gpu`, against QEMU 11.0.2 with `virtio-vga`:

- the device probes at `1af4:1050`, negotiates `VERSION_1` only, and
  `GET_DISPLAY_INFO` round-trips — `TUNIX: virtio-gpu ready, display 1280x800`
  on the serial log
- the LightDM greeter and, after logging in, the full Xfce session — wallpaper,
  panel, window manager, the welcome window — render through the scanout. Both
  were confirmed with the blit fallback compiled out, so nothing was reaching
  the screen by the old path
- the fallback itself still works: a machine with a plain VGA adapter finds no
  virtio-gpu and `drm.c` blits exactly as before
