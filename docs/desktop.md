# The graphical session

Tunix boots into [Weston](https://gitlab.freedesktop.org/wayland/weston), the
reference Wayland compositor, running as the `tunix` user. It is Void's own
`weston` package: nothing in it was built here and nothing in it is patched.

![Weston on Tunix](../screenshots/weston.png)

```sh
make run-gpu     # a window, with the desktop in it
make run         # the same image, without one
```

Ctrl+Alt+F2 leaves the desktop for a text console and Ctrl+Alt+F1 comes back.

## What is in the image

| Package | Why |
| --- | --- |
| `weston` | the compositor, its shell and its clients |
| `seatd` | hands the display and the input devices to a process that is not root |
| `mesa-dri` | the software rasteriser EGL and GL run on |
| `xkeyboard-config` | the keymaps libxkbcommon compiles |
| `dejavu-fonts-ttf` | something for the panel and the terminal to draw with |
| `xcursor-vanilla-dmz` | a cursor theme that has the drag-and-drop shapes |

`eudev` is already in the base set; it is what finds the devices.

## How it starts

Two runit services, both in `base-files/overlay/etc/sv`:

- **`seatd`** is Void's, unmodified. It opens `/dev/tty0`, puts the active
  terminal into `VT_PROCESS` mode and listens on `/run/seatd.sock`.
- **`weston`** waits for that socket to appear, makes `/run/user/1000`, and
  execs `chpst -u tunix:tunix:_seatd weston`. The account is in `_seatd`, which
  is how it reaches a display it has no permission to open itself.

`base-files/overlay/etc/xdg/weston/weston.ini` chooses the DRM backend, the
cursor theme and the terminal font. Weston's own output goes to
`/var/log/weston/` rather than to the console it is drawing over.

Terminal 1 has no `agetty`: weston takes whichever terminal is active when it
starts, and a login prompt sharing it would draw into the same cells.

## How a device is found

Weston finds its display and its input devices through libudev, and libudev
finds them by walking `/sys` and reading what udevd wrote about each one. Both
halves are the kernel's:

1. `kernel/fs/sysfs.c` publishes each device the kernel has -- the DRM card, two
   evdev nodes, the sound card -- as a directory under `/sys/devices` with a
   `uevent` file, a `subsystem` link, an entry in `/sys/class/<subsystem>` and
   one in `/sys/dev/char`.
2. `udevadm trigger` writes `add` into every one of those `uevent` files. That
   is not a write: `uevent_write()` answers it by broadcasting the device on
   `NETLINK_KOBJECT_UEVENT`, which is the whole of cold-plug.
3. udevd receives the broadcast, runs its rules, and writes the result to
   `/run/udev/data`. A device with no entry there is *uninitialised* as far as
   libudev is concerned, and libinput skips it however well it works.

Getting that chain to run end to end is most of what the kernel needed:

- **Multicast on the uevent family**, so a broadcast reaches udevd at all, and
  `SCM_CREDENTIALS` on a netlink datagram, because udev discards any message
  whose sender it cannot identify as root.
- **Unicast on the same family.** udevd hands each event to a worker by sending
  it to the port that worker bound. Dropping those left every worker asleep and
  `udevadm settle` waiting out its two-minute timeout at every boot.
- **A zero-length datagram.** A worker reports that it has finished by sending
  an empty message whose entire content is the sender's credentials. A
  zero-length write used to be treated as nothing to do.
- **`SCM_CREDENTIALS` that names the sender rather than the connection**, since
  the pid in that empty message is how udevd tells its workers apart.

## Rendering

There is no accelerated driver behind `/dev/dri/card0`, so mesa falls back to
llvmpipe and says so on the way past:

```
MESA-LOADER: failed to retrieve device information
```

That is mesa failing to identify the device from `/sys` and choosing the
software rasteriser, which is the right answer here. Weston's GL renderer then
runs on llvmpipe; `--renderer=pixman` works too and is a useful thing to try
when something looks wrong with the GL path.

## Switching away

Ctrl+Alt+F2 is taken by the kernel before it reaches evdev, so a compositor
holding a keyboard grab cannot keep the user in it. What follows is Linux's
handshake: the kernel signals whoever owns the terminal, seatd tells weston to
release the session, weston drops DRM master, and seatd answers `VT_RELDISP`.

Two things had to be fixed for that to work:

- The kernel refused to send its own signal when the processor that took the
  keyboard interrupt was idle, which is exactly when somebody presses the key.
- Handing the scanout back is not enough to bring the console back. A
  virtio-gpu that has ever been given a scanout keeps showing the virtio
  display, so the console is scanned out as a resource of its own -- see
  [virtio-gpu](virtio-gpu.md).

## What is not here

- **A display manager.** The session is one user's, started by runit, with no
  greeter in front of it.
- **XWayland.** Nothing in the image is an X client.
- **Hardware acceleration.** virgl would need the host to have a working GL
  stack and a render node, which is a property of the machine Tunix is being
  run on rather than of Tunix.
- **A second output.** DRM reports one CRTC and one connector.
