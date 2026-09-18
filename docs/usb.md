# USB

Two host controllers, one transport above them.

| Driver | Controller | What it reaches |
| --- | --- | --- |
| `kernel/drivers/usb/xhci.c` | xHCI (USB 3.x), all of them | keyboards, mice, hubs, mass storage, hot-plug |
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

## The xHCI driver

Every xHCI controller on the machine is taken, up to four, and each is named by
its position in the log (`XHCI0`, `XHCI1`). Devices are named by controller,
root port and hub ports, so `0-5.3` is controller 0, root port 5, port 3 of the
hub on it:

```
XHCI0: 1.0 at 0:3.0, 64 slots, 8 ports, 32-byte contexts, interrupts
XHCI: hub at 0-5 slot 1, 8 ports
XHCI: keyboard at 0-5.1 slot 2, endpoint 81 reporting
XHCI: mass storage at 1-2 slot 1
```

**Taking the controller.** The firmware's USB legacy support capability is
handed over first -- OS-owned is set and the BIOS given a second to let go --
and its SMIs are switched off, so the firmware stops emulating a PS/2 keyboard
behind the kernel's back. A controller with port power control has its ports
powered before anything is asked of them, and the first scan waits 200 ms for
connections to settle.

**One place for events.** Every event the controller posts is read in one
pump and handed to its owner: a command completion to the command waiting for
it, a transfer event to the endpoint that queued it, a keyboard or mouse report
straight to the input layer, a hub's change bitmap to that hub, a port change to
the port. The earlier driver read events only while waiting for a particular
one and dropped the rest, so typing while a USB stick was busy could lose the
report that re-armed the keyboard, and the keyboard fell silent.

The pump runs from the controller's interrupt -- MSI-X, or MSI when that is all
the controller offers -- from every wait, and from the input poll, so a
controller without message interrupts still works, only less promptly.

**Hubs and hot-plug.** USB 2 hubs are followed to the specification's five
tiers: ports powered, reset through class requests, and each device given its
route string and, when it is low or full speed behind a high-speed hub, the
transaction translator it hangs from. A hub's status-change endpoint and the
controller's port change events feed the same service step, which runs from the
input poll: a new connection is debounced for 100 ms, reset and enumerated; a
disconnection removes the device and everything behind it, releases any keys a
keyboard was holding, and disables its slot. A transfer to a device whose port
reported a change stops waiting at once instead of timing out.

A SuperSpeed hub is not followed. A USB 3 hub is two hubs, and its USB 2 half
appears on the matching USB 2 root port, so keyboards, mice and USB 2 disks
behind it still work; a USB 3 disk behind it does not.

**Storage.** Mass-storage devices keep the number they were first given for as
long as the machine is up, in `usb.c` as well as in the driver: a disk that is
unplugged leaves a hole that fails every command rather than shifting every
disk after it onto the wrong device. A stick plugged in after boot is
registered with the block layer, its partitions are read and its `/dev` nodes
appear, the same as one found at boot.

**When something goes wrong.** A stalled or failed endpoint is reset, its
dequeue pointer moved past the transfer that failed, and the halt cleared on
the device; a transfer that timed out has its endpoint stopped first. Keyboard
and mouse endpoints are put back this way up to eight times before the driver
gives up on them. The storage reset the transport asks for is the Bulk-Only
Mass Storage Reset with both bulk endpoints put back the same way.

**Mice.** A mouse is read the way Linux reads one, in report protocol:
`kernel/drivers/usb/hid.c` parses the interface's HID report descriptor and
finds the report ID, the buttons and the relative X, Y and wheel fields, of
whatever size and position the device chose. Boot protocol is only the fallback
for a boot mouse whose descriptor says nothing usable. Keyboards stay on boot
protocol. `support/tests/hidparse.c` checks the parser on the host against
descriptors shaped like a plain mouse, a Logitech-style receiver (report ID,
16 buttons, 12-bit motion), a 16-bit gaming mouse, a keyboard and mouse sharing
one interface, and an absolute tablet, which is refused.

`/dev/input/event1` exists whether or not a mouse was there at boot. It used to
be created only when one was, so a mouse plugged in later was enumerated by the
driver and never seen by Weston.

**Other details.** The endpoint 0 packet size of a full-speed device is read
from the first eight bytes of its descriptor and set with Evaluate Context
rather than assumed to be eight. Every keyboard and mouse interface of a device
is used, so a receiver carrying both works, and a device the driver does not
know has its interfaces listed in the log. A controller
that cannot address 64 bits gets all of its rings and buffers below 4 GiB.

`support/tests/usbtest.sh` exercises all of it on QEMU: two controllers, a
keyboard, a mouse and a full-speed disk behind a hub, a high-speed disk on the
second controller, a keyboard moved between controllers and hot-plugged behind
the hub, and a disk plugged in and pulled out during I/O, with key and mouse
events counted and every disk block written and read back. `XHCI=` picks the
controller model and its interrupt mode (`qemu-xhci,msix=off,msi=on`,
`nec-usb-xhci`, `qemu-xhci,msix=off,msi=off`); `ARCH=aarch64` runs it on `virt`.

Not tested in the emulator: endpoint stall recovery, which QEMU gives no way to
provoke, and controllers limited to 32-bit addresses.

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
- **Hubs, but only one level of them, and only for high-speed devices.** See
  below -- a rate-matching hub is not optional on the machines this exists for.
  A slower device behind a hub would need split transactions, which this driver
  does not do, and is skipped with a line saying so.
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

## The hub on the root port

Intel chipsets of the BIOS era put a **rate-matching hub** on the root port of
each EHCI controller and hang every physical socket off it. On such a machine
the root ports each hold exactly one device, that device is the hub, and no
amount of correct root-port handling finds a disk. What it looks like from the
outside is this, from a real machine:

```
USB: ehci at 0:1a.0
USB: ehci at 0:1d.0
EHCI: 1.0 at d9105c00, 3 ports, async schedule running
EHCI: 1.0 at d9105800, 3 ports, async schedule running
EHCI: port 1 idle, status 1007
EHCI: port 2 idle, status 1000
EHCI: port 3 idle, status 1000
```

`1007` is connected, enabled and powered: the port reset worked and there is a
device on it. Both controllers, port 1, the same answer -- that shape is the
hub.

So the driver follows it. Only the management of the hub is needed, not split
transactions: a high-speed device behind a high-speed hub is addressed
directly and the hub is transparent to its transfers. Ports are powered, reset
and read through class requests on the control pipe rather than through a
register, and the sequence is otherwise the root-port one. The hub is
configured first -- a device in the address state is not required to answer
anything but the standard requests, and every port operation is a class one.

One level. A hub behind a hub is not something a chipset does to itself, and
following it would need a queue this driver does not have.

**This path is not tested in the emulator.** QEMU models only a full-speed hub,
which cannot attach to an EHCI bus at all, so there is no way to stand a
high-speed hub up in front of it here. Everything else in this file is
verified against QEMU; the hub code is verified against the specification and
against one machine.

## A halted endpoint has to be cleared

A device halts an endpoint to refuse something, and it stays halted. Every
transfer after it fails, and because each one waits out its timeout first, the
machine is not frozen but crawling -- which from the front is the same thing,
and is what a real machine did after userland had started: the trace stopped on
an `openat`, which is the dynamic loader opening a library, which is a read.

So a failed transfer now looks at the descriptor the controller handed back,
and clears the halt when there is one. The device resets its data toggle as it
does that, so the software toggle is reset with it -- leaving the two
disagreeing is a second, permanent version of the same failure, and the first
version of this driver did exactly that: it reset its own toggle and never told
the device.

It is reset when the halt is cleared and at no other time, because clearing
the halt is the only thing that resets the device's. A transfer that merely
timed out moved nothing at either end; resetting ours there is how the two come
to disagree, and once they do every transfer after it fails the same way. What
that looked like was a 31-byte command retried with the other toggle for ever
--

```
EHCI: bulk out endpoint 2 failed, token 1f8c80
EHCI: bulk out endpoint 2 failed, token 801f8c80
EXT2: write-back failed for current
```

-- six thousand times in a five-minute run, because writes are the transfers
with two OUT stages back to back. Reads carried on working the whole time,
which is why the machine looked fine and could not save anything.

The toggle also advances by the packets actually moved rather than the packets
asked for. A device is allowed to end a transfer early and says how much it
left behind; counting the request instead of the answer desynchronises the
endpoint the first time one does.

Failures say so, up to a few times:

```
EHCI: bulk in endpoint 2 failed, token 40008d80
```

## Waking a schedule that stopped

EHCI notices an asynchronous schedule with nothing to do in it and stops
walking it, and there is no doorbell to ring. Work put into a queue head that
is already on the ring can therefore simply never start. What comes back is a
descriptor still marked active with none of its bytes moved:

```
EHCI: bulk out endpoint 2 failed, token 1f8c80
```

0x80 is active and 0x1f is the whole 31-byte command still waiting. A transfer
that has not started after 20 ms turns the schedule off and on again, which is
what restarts the traversal.

Once, and late, deliberately. Turning the schedule off is not free for a
transfer already under way: kicking every couple of milliseconds instead stops
transfers finishing at all, and the boot does not get past its first seconds.

## A failed command is retried, after the device is put back in order

A transfer can fail without the device having seen anything: the controller
drops a command that never left its schedule, and what comes back is a
descriptor with all 31 bytes of the wrapper still waiting. That is not a disk
problem, and failing an install over it is the wrong answer -- a 677 MB
download is a hundred thousand writes and one of them going astray ended the
whole thing with `Operation not supported`.

But the two kinds of failure look alike from the transport and are not. The
other kind is a command that failed *after* its wrapper went out, and the
device is then waiting for data or holding a status nobody collected. Sending
the next command into that is how one failure becomes every failure: the device
reads the new wrapper as the data it was still expecting, and nothing lines up
again. Retrying without that distinction turned a handful of failures into a
hundred and twenty-nine thousand.

Telling them apart from the transport is not possible, so the class reset runs
before every retry: a Bulk-Only Mass Storage Reset addressed to the interface,
then the halt cleared on both bulk endpoints, then both toggles back to zero.
It is what the specification asks for and it is paid only on a path that has
already gone wrong.

The log is capped at eight lines. It is painted on the console, so a message
per failed block is not a diagnostic but a second failure on top of the first.

## The shape of the driver

The asynchronous schedule and nothing else. Two queue heads sit in a ring --
an empty head marked as the head of the reclamation list, and a working one --
and **neither is ever taken out of it again**. A transfer is started by
pointing the working queue head at a chain of descriptors and finished by
pointing it back at nothing. There is no doorbell in EHCI, so the controller
walks the ring on its own and every transfer ends in a poll on the descriptor's
own status byte rather than on an event ring the way xHCI's does.

The ring being permanent is the important half, and it was not how this started.
Linking the working queue head in for the duration of a transfer and unlinking
it after looks obviously correct and is not: the specification will not let
software touch a queue head it has unlinked until the controller has
acknowledged the interrupt-on-async-advance doorbell, because the controller
caches queue heads and is very likely still following the one just removed.
Doing it per transfer is that mistake per transfer. An emulated controller
forgives it completely. On a real one the boot reached the point of loading
init off the stick and stopped there.

An idle queue head with no descriptors is skipped, so leaving both linked
forever costs nothing and removes the question.

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
