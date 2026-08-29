# USB

Two host controllers, one transport above them.

| Driver | Controller | What it reaches |
| --- | --- | --- |
| `kernel/drivers/usb/xhci.c` | xHCI (USB 3.x) | keyboards, mice, mass storage |
| `kernel/drivers/usb/ehci.c` | EHCI (USB 2.0), all of them | high-speed mass storage |

`kernel/drivers/usb/usb.c` is the seam between them and
`kernel/drivers/usb/usb_storage.c`, which speaks the bulk-only transport and
SCSI on top of it. A host offers two things -- how many mass-storage devices it
found, and a synchronous bulk transfer to one of them -- and the transport
numbers the devices of every registered controller in one flat list. Nothing
above a controller learns which kind it is.

The four kinds of USB host controller share a PCI class and subclass and are
told apart only by the programming interface, and a machine can have several --
old Intel chipsets split their ports across *two* EHCI controllers, and a
machine with xHCI usually has an EHCI beside it. So neither driver looks up
"serial bus, USB" and takes the answer: `pci_find_nth_class()` walks them all
and each driver keeps the ones it recognises. Stopping at the first was worth
two failed attempts on real hardware -- the stick was on the half of the ports
belonging to the controller that was never looked at.

Every USB controller found is logged whether or not it can be used:

```
USB: ehci at 0:1a.7
USB: ehci at 0:1d.7
USB: xhci at 0:14.0
```

"there is no EHCI here" and "the EHCI here found nothing" are different
answers, and on a machine with no serial port this line is the only place the
difference shows.

## Why EHCI exists here

The bootloader reads the kernel off a USB stick through the firmware and then
hands over. From that moment the stick is reachable only by a driver the kernel
has, so on a machine older than xHCI the root filesystem was on a disk nothing
here could see. What that looked like was a boot that ended in

```
block devices: sda sda1 sda2 sda3
*** KERNEL PANIC ***
root filesystem mount failed
```

with the machine's internal disk listed and the stick it had just been booted
from absent.

## What the EHCI driver does not do

- **High-speed devices only.** A full- or low-speed device is handed to the
  companion UHCI or OHCI controller by writing Port Owner, which is what the
  specification asks for. There is no companion driver, so such a device is not
  reached. It does not cost anything worth having: a USB stick is high speed,
  and a keyboard is behind the firmware's legacy emulation long before this.
- **No hubs and no split transactions.** A device behind a hub is not found.
- **No HID.** See above -- the devices EHCI would have to talk to at full speed
  are the ones it deliberately gives away.

## Ports have to be turned on before they can be asked

A controller with port power control comes out of a reset with its ports
unpowered, and an unpowered port reports no connection. Asking whether
something is plugged in before turning the port on is therefore a question with
one possible answer, and it is the wrong one. The first version of this driver
did exactly that: it powered the port inside `reset_port()`, *after* the check
that returned early when nothing was connected.

It passed every test, because the emulated controller powers its own ports and
never says no. On real hardware every port on both controllers came back empty.

So `start_controller()` powers all of them -- clearing Port Owner at the same
time, since the firmware hands ports to the companion controller for its legacy
emulation and a reset does not always take them back -- and `ehci_init()` waits
the 100 ms debounce once, for every controller at once, before anything looks.

When a controller finds nothing, it prints what its ports actually read:

```
EHCI: port 1 idle, status 1000
```

A port with nothing in it, a port owned by the companion and a port that
refused to enable are three different problems that look identical from
outside. PORTSC tells them apart.

## The shape of the driver

The asynchronous schedule and nothing else. A queue head sits in a ring that
points at itself; a second queue head carrying the transfer descriptors is
linked in behind it for the duration of one transfer and unlinked again after.
There is no doorbell in EHCI -- the controller walks the ring on its own, which
is why every transfer ends in a poll on the descriptor's own status byte rather
than on an event ring the way xHCI's does.

Everything the controller reads lives in one page below 4 GiB. Both halves of
that are deliberate: a queue head is 84 bytes rather than the 64 it looks like,
so the offsets inside the page are spaced far enough apart that an overlap is
not possible -- the first version of this driver had one, and the symptom was a
controller that ran, reported its schedule enabled, and never touched a single
descriptor. And every pointer the controller follows is 32 bits with a shared
high half, so a structure above 4 GiB is not something it can be told about.

Before any of it, the controller has to be taken from the firmware: EHCI's
legacy-support capability lives in PCI config space and the BIOS owns the
controller until the handshake in `release_from_firmware()` completes.
