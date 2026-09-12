# Roadmap

What is worth doing next, in the order the system would notice. Not a wishlist:
each item is something that is missing and that something real wants.

## 1. Packet capture

`AF_PACKET` now carries DHCP traffic correctly and dhcpcd configures `eth0`.
The next networking tool to exercise is `tcpdump`, including its BPF filters
and packet metadata under sustained traffic.

## 2. The rest of Xorg's world, or a bigger Wayland one

Weston runs, which means udev, seatd, libinput, DRM, EGL and GBM all work well
enough for a compositor. The next thing to try is a desktop rather than a
reference compositor -- Void's own Xorg and Xfce packages, or a Wayland session
with a toolkit under it. Neither is known to be blocked; neither has been tried
since the ports tree went.

## 3. cgroups, or an honest refusal

`mount -t cgroup2` fails on every boot. Nothing here needs it, but a fair
amount of software checks. Deciding whether to implement a hierarchy or to
answer the check properly is a decision that has been deferred by accident.

## 4. A second filesystem worth writing to

ext3 with an unused journal, no extents and a 16 GiB ceiling is enough for a
root and not much else. Either ext4 read support or replaying and writing the
journal that is already there is the next step, and the first is the cheaper
one: it makes the disks Linux writes readable here.

## 5. Sound, verified again

The driver works and was last exercised by programs built against the kernel's
own libc, which no longer exist. Void's `alsa-utils` is one package and would
say whether the ioctl surface is right by Linux's standards rather than by ours.
Input no longer needs this: libinput drives both evdev nodes under weston.

## 6. Hotplug that means something

Uevents are sent and udevd acts on them, but every device this kernel has
exists from boot and is announced once. Nothing appears or disappears later, so
the half of udev that matters most on real hardware has never run.
