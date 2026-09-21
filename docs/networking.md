# Networking

Tunix drives virtio-net and RTL8139 adapters, one at a time, with ARP, IPv4,
ICMP, UDP, raw and packet sockets,
`AF_NETLINK`/rtnetlink behind iproute2's `ip` and `ss`, and TCP. Both ends of
TCP work: a process can dial out, and a process can wait to be dialled.

## Loopback

Packets addressed to `127.0.0.0/8`, or to the machine's own address, never
reach the adapter. `net_send_ipv4()` puts them on a queue and `net_poll()`
hands them back to the receive path.

The queue is the whole design. Delivering a local packet inline would recurse
— handling a SYN sends a SYN-ACK, which is another local packet, from inside
the handler still running — and no depth limit makes that safe. A queue breaks
the cycle instead of bounding it. It holds 40 packets because a sender fills
the peer's window before waiting for an acknowledgement (16 segments at a
16 KiB window and 1 KiB MSS), and a queue shorter than a window drops what a
wire would have carried, turning every burst into a retransmit timeout.

Loopback is drained before the link is checked, so it works on a machine with
no adapter at all.

The source address is picked per destination (`net_source_for()`): a packet to
`127.0.0.1` goes out *from* `127.0.0.1`. This matters more than it looks —
the reply is matched by four-tuple, so a client that stamped the adapter's
address on a loopback connection could not recognise its own SYN-ACK.

## The adapters

QEMU gets a modern-only virtio-net PCI function by default. The driver
negotiates the device MAC, keeps 128 receive buffers in queue 0, sends through
queue 1, and binds both queues to one MSI-X vector. Each buffer starts with the
12-byte version-1 virtio header and carries one Ethernet frame after it. The
interrupt acknowledges the device; `net_poll()` takes completed buffers from
the used ring and enters the stack where it is safe to do so.

RTL8139 remains the fallback for real hardware and for
`QEMU_NET="-netdev user,id=net0 -device rtl8139,netdev=net0"`, and it is a
module (`kernel/modules/x86_64/rtl8139.c`): udev loads it from the card's PCI
modalias and it registers itself, so `eth0` exists from the moment a driver
claims a card rather than from boot. Either driver registers a `struct
net_adapter` and the stack above them never names one. Its receive path is the
one below.

`kernel/modules/atl1c.c` drives the Atheros AR8131/AR8132 gigabit controller,
which is the wired port on a lot of 2010-era laptops and the reason it exists
here: it was the first card a Tunix hardware report found with no driver behind
it. It is polled like the RTL8139 -- `net_poll()` drains it -- with 64 receive
slots of 1536 bytes each, a 16-entry transmit ring, and a link watchdog that
re-reads the PHY twice a second and re-programs the MAC when the speed or duplex
changes, so a cable plugged in after boot still works. Interrupts are the
obvious next step; nothing else about the driver assumes polling.

### The chip a laptop hands you is asleep

A PCIe card on a laptop does not arrive in the state a datasheet describes. The
BIOS has already put the link into ASPM, and the AR8131 wakes with its internal
clocks gated, so a driver that programs nothing but rings and the MAC can bring
the link up, print a speed, and then move no frames at all -- which is exactly
what the first report from the laptop showed: `atl1c` bound, link up, every
counter zero.

Three registers decide it, and the driver writes all three before it carries
traffic:

- `PM_CTRL` (0x12F8), the MAC's own view of ASPM. With the link up the driver
  turns L0s and L1 off, zeroes the L1 entry timer and enables the SerDes L1
  block; with the link down it parks the chip the other way round, clock switch
  on and the SerDes block off. It is re-written on every link change, because
  the right setting depends on whether there is a cable.
- `CLK_GATING_CTRL` (0x1814), which is cleared: on this part the clocks stay on.
- `PCIE_PHYMISC` (0x1000) and `LTSSM_ID_CTRL` (0x12FC), the PCIe receiver-detect
  and LTSSM bits, plus `MASTER_CTRL`'s clock-select bit, all set the way Linux's
  own driver sets them at probe.

None of this can be found in an emulator: QEMU has no ASPM and no clocks to
gate. The register numbers and the bit meanings were taken from the hardware
facts in Linux's atl1c, and the model test below now starts the chip the way a
laptop does -- gated and in ASPM -- so a driver that skips the wake-up fails the
test instead of the machine.

### Somebody has to ask a card that cannot ask you

The RTL8139 and virtio-net raise an interrupt when a frame lands; the AR8131
driver does not use one, so the only way a frame is ever seen is somebody
calling `poll`. Every call sat inside the stack -- a send, a receive, an ARP
resolution, a socket becoming ready -- which means the card was serviced only
while a process was busy with a socket. A machine where nothing has opened one
yet still has a link to watch, a receive ring filling behind it and, in this
driver's case, a link watchdog that never runs: the first report from the
laptop came back with `link_changes 0`, because between udev loading the module
and the report being written, nothing in userspace had touched the network.

So `timer_irq()` calls `net_tick()`, beside the input and sound polling that
were already there. It does nothing at all when the adapter has an interrupt
vector, and otherwise drains the card and runs its watchdog 250 times a second,
whether or not anybody is listening.

`nonetirq` on the kernel command line makes an adapter that *does* interrupt
behave like one that does not, which is the only way to run the polled path
under an emulator. It found that the RTL8139's receive queue was allocated in
`enable_interrupt()` and nowhere else, so with interrupts off the driver threw
every frame away: dhcpcd got no lease and fell back to a link-local address,
which is the same "no gateway" shape the laptop's report showed. The queue is
allocated at probe now, and `EXTRA_CMDLINE=nonetirq support/tests/moduletest.sh`
passes all 87 checks -- DHCP included -- on a card whose interrupts are gone.

## A driver with no emulator

QEMU has no AR8131, so `support/tests/atl1ctest.c` is the test: it compiles the
driver's own source for the host and runs it against a model of the chip. The
model is a register window and a thread that behaves like the hardware -- it
serves MDIO transactions from a fake PHY, clears the reset bit, consumes
transmit descriptors and posts receive ones -- and the DMA arena is mapped low
so the 32-bit addresses the driver programs are addresses the model can follow.

That is enough to check the things a wrong driver gets wrong: the station
address it read -- from the registers, and from the EEPROM when the registers
are empty -- the ring base addresses and sizes it programmed, that the MAC and
both queues ended up enabled at the speed the PHY reported, that frames handed
to `transmit` arrive byte for byte, that received frames come back the same way
with their slots returned to the hardware, that a full transmit ring refuses
work rather than overwriting it, and that a link change re-programs the MAC.
`make atl1ctest` runs it in under a second, and CI runs it on every push.

The model is deliberately as unforgiving as the chip. It moves no frame in
either direction unless bus mastering is on and both the queue and the MAC half
are enabled, so a forgotten enable is a failed test rather than a dead laptop;
it wakes with its clocks gated and ASPM on, so the wake-up above is tested; and
it consumes as many receive slots as the descriptor it posts claims. On top of
that the test drives what a real cable does over an afternoon: 240 frames in and
60 out, several times round both rings; descriptors that report a checksum
error, an 802.3 length error, an impossible length, an index past the ring and a
frame spanning two slots, each of which has to be dropped, counted, and leave
the ring in step for the good frame behind it; a link that goes down and comes
back while traffic is flowing; and a transmit attempted after the driver has
been removed.

What the model cannot check is whether those register offsets are the ones the
real chip answers to. That part was read off a Core i5 laptop's own hardware
report.

## How an RTL8139 frame gets in

The card interrupts, and the handler does exactly one thing: it empties the
card's ring into a queue of 128 frames. It does not touch the network stack.

It cannot. An interrupt arrives inside whatever the processor was doing, which
may be a system call already halfway through that same stack, and the kernel
lock does not separate the two -- the handler runs *inside* the lock its victim
is holding. So the work is split by deadline. Getting frames out of the card
before the ring wraps over them has one; parsing them does not.

`net_poll()` then hands queued frames to the stack at syscall time, as it always
did, and sweeps the card itself when there is no interrupt to be had -- a
machine where the line could not be routed still receives, exactly as it did
before.

Which line that is takes a guess. A PCI interrupt pin reaches an IOAPIC input
that ACPI's `_PRT` describes, in AML, which this kernel does not interpret; the
number in config space is the one the 8259 would have wanted, and on q35 it is
not the IOAPIC's. So `rtl8139_enable_interrupt()` routes every input the card
could be on -- the config-space line and 16 through 19 -- to one handler that
reads the card's status register and returns when the card has nothing to say.
Sharing a level-triggered pin means doing that anyway.

Routing happens after `apic_init()`, not in `net_init()`. The adapter is set up
early, before there is an interrupt controller to route through, and routing
there fails silently and permanently -- which it did, until the two were split.

Measured: 15.5 MB over HTTP raises about 10,000 interrupts, one per frame, and
takes the same 25 seconds it took when the ring was swept at syscall time. The
gain is not throughput for a program sitting in `recv()`; it is that frames stop
depending on one being called.

## TCP servers

`bind`, `listen` and `accept` work on `AF_INET` sockets.

A listening socket has no control block of its own. When a SYN arrives for its
port, the stack creates a *separate* socket in `SYN_RECEIVED` and answers the
SYN-ACK from there, so the rest of the handshake is matched by four-tuple like
any other connection. That socket joins a list on the listener and becomes
visible to `accept()` only once the handshake completes; `accept()` hands it
over with the reference the stack was holding.

- The backlog caps how many connections a listener may hold un-accepted (16 at
  most). A SYN arriving at a full backlog is dropped, not refused, so the peer
  retransmits into a queue that may have drained by then.
- A blocking `accept()` waits; only a non-blocking socket answers `EAGAIN`.
- `poll`/`epoll` report a listener readable when a connection is ready to take,
  which is what every server waits on.
- Closing a listener resets and frees every connection still on its list —
  nobody will ever accept them.
- Connections that die before being accepted are reaped by the same timer sweep
  that retransmits, since no descriptor exists to notice them.

`/proc/net/tcp` lists listeners as state `0A` and half-open connections as
`03`.

## What glibc's resolver wants

Nothing here resolves a name itself: `/etc/resolv.conf` names QEMU's resolver
and glibc does the rest over UDP. That turned out to be a stricter test than
sending a packet, because the resolver treats two things as fatal that look
optional from the kernel side, and neither of them fails loudly.

- **`setsockopt(SOL_IP, IP_RECVERR)`**, which it sets on every nameserver
  socket it opens so that an ICMP port-unreachable turns into a quick error
  rather than a timeout. A socket that refuses the option is closed and the
  lookup reported as failed — before a single query is sent. Refusing it made
  every name on the machine unresolvable; xbps called that "Transient resolver
  failure" and curl called it "Could not resolve host".
- **`sendmmsg`**, which it uses to put the A and the AAAA query for one name
  into a single call. There is no fallback: `ENOSYS` there fails the lookup the
  same silent way. `getent hosts` asks for one family and so never took that
  path, which made the failure look like it depended on the program.

Nothing is put on a socket's error queue, so `IP_RECVERR` is accepted and
answers honestly: there is never anything to read. `sendmmsg` is a loop around
`sendmsg`, which is what it is on Linux too.

## Checking it

`tcp-test` on the image runs a server and a forked client against each other
over `127.0.0.1`: accept, peer address, a request/response exchange, a 64 KiB
transfer (four times the receive ring, so it only completes if the window
opens and closes correctly), a refused connection to a dead port, and a UDP
round trip. `python-test`'s `inet sockets` check does the same through
CPython's own socket module.

### One address, and who is allowed to take it away

The stack holds a single IPv4 address, so `RTM_DELADDR` used to mean "there is
no address any more" whatever address the message named. dhcpcd does not work
that way: while it waits for a lease it configures a link-local address, and
when the lease arrives it adds the real one and *then* deletes the old one. The
delete took the new address with it, and the machine was left with a default
route to a gateway it had learned, a route table whose link entry was
`0.0.0.0/0.0.0.0`, and `ip addr` showing nothing on `eth0` -- which is exactly
what the laptop's report printed after a successful DHCP exchange.

A delete that names an address other than the one held is now ignored.
`moduletest` deletes `192.0.2.9/24` from `eth0` after dhcpcd has finished and
checks the lease is still there, which fails on a kernel without the fix.

`/proc/net/dev` counts bytes as well as packets now, for the same reason: a
report that says 40 packets and 0 bytes reads like a broken driver.

### Pinging without being root

`ping` on the image is not setuid; it carries `cap_net_raw=p`, and Tunix has no
file capabilities, so it runs with nothing. Linux solved that in 2011 with the
unprivileged ICMP socket, and that is what this stack offers:
`socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)` is open to anybody.

It is not a raw socket with the checks removed. The kernel owns the identifier:
`bind()` with port 0 allocates one, `getsockname()` reports it -- which is how
`ping` learns what to match replies against -- and every echo request the socket
sends has its id overwritten and its checksum recomputed before it leaves. Only
an echo *request* may be sent, and only an echo reply carrying that id is
delivered back, with the IP header stripped, so one program's replies cannot
land in another's queue.

With that in place `SOCK_RAW` and `AF_PACKET` now need root, where they were
open to every user -- the looseness that made `ping` work before. dhcpcd, which
is the other user of `AF_PACKET` on the image, runs as root.

`ICMP_FILTER` works too. It is a bitmask of the ICMP types a socket does *not*
want; `ping` sets it to everything but the replies it cares about and used to
get a warning back:

```
ping: WARNING: setsockopt(ICMP_FILTER): Operation not supported
```

`proctest` runs the whole sequence as uid 1000: read the capabilities and write
them back, be refused a raw socket, open an ICMP socket, bind it, learn its id,
and ping `127.0.0.1` through the loopback path -- checking that the reply comes
back with the kernel's id, the sequence number that was sent, and the payload
unchanged.

## Limits

- One adapter and one IPv4 address, normally configured by dhcpcd.
- No IPv6.
- TCP has no MSS option, no window scaling, no selective acknowledgement, and
  no out-of-order reassembly — a segment arriving early is re-acknowledged and
  dropped rather than held.
- 32 sockets in total, across every family.
- No `MSG_ERRQUEUE`, so an ICMP error is never reported to the socket that
  caused it, and no `recvmmsg` to go with `sendmmsg`.
- An ICMP socket carries no errors: a destination that answers with
  "unreachable" is silence rather than a message, because `MSG_ERRQUEUE` is not
  modelled.

## Socket options refused on principle

Twice now an option this stack does not model has been refused, and twice that
broke something with nothing to do with the option.

`IP_RECVERR` was the first. glibc's resolver sets it on every nameserver socket
and treats a refusal as fatal, so every name on the machine became unresolvable
without a packet reaching the wire -- `Transient resolver failure` from xbps,
`Could not resolve host` from curl.

`TCP_NODELAY` was the second. libfetch sets it on every connection it opens, and
a 677 MB download died three quarters of the way through with `Operation not
supported`: the option is refused on every connection, and becomes fatal on the
one libfetch opens to resume. It is accepted now, and truthfully rather than
conveniently -- there is no Nagle here. A segment goes out when the caller
writes it and small writes are never held back waiting for company, so the
option describes what already happens.

The rule that comes out of both: an option that only describes behaviour should
be accepted when the behaviour is already what it asks for, and refused only
when honouring it would matter and the stack cannot.

And a refusal names itself now, once per level and option:

```
INET: setsockopt level 6 option 1 refused
```

Working out that the last two were `IP_RECVERR` and `TCP_NODELAY` cost a boot
each. The number is free to print and it is the whole diagnosis.
