# Roadmap

What is worth doing next, in the order the system would notice. Not a wishlist:
each item is something that is missing and that something real wants.

## 1. A graphical session again

The desktop was the reason for the ports tree, and it went with it. Bringing it
back means installing Void's own Xorg and Xfce packages -- which is a much
smaller job than building them was, and a much better test, because those
binaries are not compiled against this kernel's quirks.

What is known to be in the way:

- **`udevd` does not run.** Xorg and libinput find their devices through udev,
  and udev finds them through netlink uevents the kernel does not send.
- **A lost futex wakeup.** Seen before, under the old ports build, hanging Xorg.
  It was never root-caused and there is no reason to think it went away.

## 2. Uevents over netlink

Which is item 1's blocker, and useful on its own: it is how anything learns that
a device appeared. The netlink socket exists and rtnetlink works; what is
missing is the `NETLINK_KOBJECT_UEVENT` family and a kernel-side notification
when devfs attaches a node.

## 3. A DHCP client

`/etc/rc.local` puts a static address on `eth0` because dhcpcd wants an
`AF_PACKET` socket and the network stack has no concept of one. Packet sockets
would also make `tcpdump` work, which is the tool this stack most wants when
something goes wrong.

## 4. cgroups, or an honest refusal

`mount -t cgroup2` fails on every boot. Nothing here needs it, but a fair
amount of software checks. Deciding whether to implement a hierarchy or to
answer the check properly is a decision that has been deferred by accident.

## 5. A second filesystem worth writing to

ext2 with no journal, no extents and a 16 GiB ceiling is enough for a root and
not much else. Either ext4 read support or a real journal is the next step, and
the first is the cheaper one: it makes the disks Linux writes readable here.

## 6. Sound and input, verified again

Both drivers work and both were last exercised by programs built against the
kernel's own libc, which no longer exist. Void's `alsa-utils` and `evtest` are
one package each and would say whether the ioctl surfaces are right by Linux's
standards rather than by ours.
