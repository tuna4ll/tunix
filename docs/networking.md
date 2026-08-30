# Networking

Tunix has one adapter (RTL8139), ARP, IPv4, ICMP, UDP, raw and packet sockets,
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

## Limits

- One adapter, one address. No DHCP client: the address comes up as QEMU user
  networking's default and `ip` can replace it.
- No IPv6.
- TCP has no MSS option, no window scaling, no selective acknowledgement, and
  no out-of-order reassembly — a segment arriving early is re-acknowledged and
  dropped rather than held.
- 32 sockets in total, across every family.
- No `MSG_ERRQUEUE`, so an ICMP error is never reported to the socket that
  caused it, and no `recvmmsg` to go with `sendmmsg`.
- `ping` works but warns: it asks for `ICMP_FILTER` to choose which ICMP types
  reach it, is refused, and filters in userspace instead.

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
