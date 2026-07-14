#include <stddef.h>
#include <stdint.h>
#include "../include/heap.h"
#include "../include/kstring.h"
#include "../include/time.h"
#include "../include/net/inet_socket.h"
#include "../include/net/net.h"

#define MAX_INET_SOCKETS 32
#define SOCKET_QUEUE 8
#define SOCKET_PACKET_MAX 2048
#define EAGAIN 11
#define ENOMEM 12
#define EFAULT 14
#define EINVAL 22
#define ENOTTY 25
#define EDESTADDRREQ 89
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETDOWN 100
#define ENOTCONN 107
#define EPIPE 32
#define EMSGSIZE 90
#define ECONNRESET 104
#define EISCONN 106
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EINPROGRESS 115

/* TCP tuning. Rings are per-connection byte streams; MSS caps segment payload
   to what net_send_tcp accepts. Timers use time_uptime_ns() deadlines. */
#define TCP_RING 8192U
#define TCP_MSS 1024U
#define TCP_RTO_INIT_NS   200000000ULL   /* 200 ms  */
#define TCP_RTO_MAX_NS   4000000000ULL   /* 4 s     */
#define TCP_MAX_RETRIES  8
#define TCP_TIME_WAIT_NS 10000000000ULL  /* 10 s (shortened 2*MSL) */
#define TCP_ORPHAN_NS    30000000000ULL  /* max lifetime of an orphaned TCB */

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSING,
    TCP_TIME_WAIT,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK
};

struct tcp_control_block {
    int state;
    uint32_t iss;                 /* initial send sequence            */
    uint32_t snd_una;             /* oldest unacknowledged sequence   */
    uint32_t snd_nxt;             /* next sequence to send            */
    uint16_t snd_wnd;             /* peer's advertised window         */
    uint32_t irs;                 /* initial receive sequence         */
    uint32_t rcv_nxt;             /* next expected sequence           */
    int fin_sent;
    int fin_acked;
    int peer_fin;
    int pending_error;            /* negative errno, delivered once   */
    uint8_t tx[TCP_RING];         /* unacked + unsent outbound bytes  */
    size_t tx_len;                /* total buffered outbound bytes    */
    size_t tx_sent;              /* sent-but-unacked prefix of tx    */
    uint8_t rx[TCP_RING];         /* in-order received inbound bytes  */
    size_t rx_head;
    size_t rx_len;
    uint64_t rto_ns;
    uint64_t rto_deadline_ns;     /* 0 = retransmit timer disarmed    */
    unsigned retransmit_count;
    uint64_t time_wait_deadline_ns;
    uint64_t orphan_deadline_ns;
};

#define MSG_PEEK 0x02
#define MSG_DONTWAIT 0x40
#define SOL_SOCKET 1
#define SO_ERROR 4
#define SO_BROADCAST 6
#define SO_RCVBUF 8
#define SO_SNDBUF 7
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_BINDTODEVICE 25
#define SO_ATTACH_FILTER 26
#define IPPROTO_IP 0
#define IP_HDRINCL 3
#define IP_TTL 2

#define SIOCADDRT 0x890BU
#define SIOCDELRT 0x890CU
#define SIOCGIFFLAGS 0x8913U
#define SIOCSIFFLAGS 0x8914U
#define SIOCGIFADDR 0x8915U
#define SIOCSIFADDR 0x8916U
#define SIOCGIFBRDADDR 0x8919U
#define SIOCGIFNETMASK 0x891BU
#define SIOCSIFNETMASK 0x891CU
#define SIOCGIFMTU 0x8921U
#define SIOCGIFHWADDR 0x8927U
#define SIOCGIFINDEX 0x8933U

#define IFF_UP 0x0001
#define IFF_BROADCAST 0x0002
#define IFF_RUNNING 0x0040
#define IFF_MULTICAST 0x1000

struct queued_packet {
    size_t length;
    uint8_t data[SOCKET_PACKET_MAX];
    uint8_t address[32];
    size_t address_length;
};

struct inet_socket {
    int refs;
    int domain;
    int type;
    int protocol;
    uint32_t local_address;
    uint16_t local_port;
    uint32_t peer_address;
    uint16_t peer_port;
    int connected;
    int bound;
    int read_shutdown;
    int write_shutdown;
    int broadcast;
    int header_included;
    uint8_t ttl;
    int orphan;                          /* fd closed but TCB still closing */
    struct tcp_control_block *tcp;       /* non-NULL only for SOCK_STREAM   */
    struct queued_packet queue[SOCKET_QUEUE];
    unsigned queue_head;
    unsigned queue_tail;
    unsigned queue_count;
};

static struct inet_socket *sockets[MAX_INET_SOCKETS];
static uint16_t next_ephemeral = 49152;

static int register_socket(struct inet_socket *socket) {
    for (unsigned i = 0; i < MAX_INET_SOCKETS; i++) {
        if (!sockets[i]) { sockets[i] = socket; return 0; }
    }
    return -1;
}

static void unregister_socket(struct inet_socket *socket) {
    for (unsigned i = 0; i < MAX_INET_SOCKETS; i++)
        if (sockets[i] == socket) sockets[i] = NULL;
}

static uint16_t allocate_port(void) {
    for (unsigned attempt = 0; attempt < 16384; attempt++) {
        uint16_t candidate = next_ephemeral++;
        if (next_ephemeral < 49152) next_ephemeral = 49152;
        int used = 0;
        for (unsigned i = 0; i < MAX_INET_SOCKETS; i++)
            if (sockets[i] && sockets[i]->domain == TUNIX_AF_INET &&
                sockets[i]->local_port == candidate) used = 1;
        if (!used) return candidate;
    }
    return 0;
}

/* ------------------------------------------------------------------ TCP --- */

static uint32_t tcp_iss_salt;

static int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static int seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

static uint32_t tcp_generate_iss(void) {
    tcp_iss_salt += 0x9E3779B9U;
    return (uint32_t)(time_uptime_ns() >> 6) ^ tcp_iss_salt;
}

static uint16_t tcp_rx_window(const struct tcp_control_block *tcp) {
    size_t space = TCP_RING - tcp->rx_len;
    return space > 0xFFFFU ? 0xFFFFU : (uint16_t)space;
}

static void tcp_arm_rto(struct tcp_control_block *tcp) {
    if (!tcp->rto_ns) tcp->rto_ns = TCP_RTO_INIT_NS;
    tcp->rto_deadline_ns = time_uptime_ns() + tcp->rto_ns;
}

/* Emit one segment with the current receive window/ack. Sequence bookkeeping
   is the caller's responsibility. */
static void tcp_transmit(struct inet_socket *s, uint32_t seq, uint8_t flags,
                         const uint8_t *data, size_t length) {
    struct tcp_control_block *tcp = s->tcp;
    net_send_tcp(s->local_address, s->local_port, s->peer_address, s->peer_port,
                 seq, tcp->rcv_nxt, flags, tcp_rx_window(tcp), data, length);
}

static void tcp_send_ack(struct inet_socket *s) {
    tcp_transmit(s, s->tcp->snd_nxt, TCP_ACK, NULL, 0);
}

/* Push unsent data respecting the peer's advertised window and the MSS. */
static void tcp_output(struct inet_socket *s) {
    struct tcp_control_block *tcp = s->tcp;
    if (tcp->state != TCP_ESTABLISHED && tcp->state != TCP_CLOSE_WAIT) return;
    while (tcp->tx_sent < tcp->tx_len) {
        uint32_t window = tcp->snd_wnd ? tcp->snd_wnd : 1U; /* zero-window probe */
        if (tcp->tx_sent >= window) break;
        size_t room = (size_t)window - tcp->tx_sent;
        size_t chunk = tcp->tx_len - tcp->tx_sent;
        if (chunk > TCP_MSS) chunk = TCP_MSS;
        if (chunk > room) chunk = room;
        if (!chunk) break;
        uint32_t seq = tcp->snd_una + (uint32_t)tcp->tx_sent;
        tcp_transmit(s, seq, TCP_ACK | TCP_PSH, tcp->tx + tcp->tx_sent, chunk);
        tcp->tx_sent += chunk;
        tcp->snd_nxt = tcp->snd_una + (uint32_t)tcp->tx_sent;
        tcp_arm_rto(tcp);
    }
}

static void tcp_free(struct inet_socket *socket) {
    unregister_socket(socket);
    if (socket->tcp) kfree(socket->tcp);
    kfree(socket);
}

/* Queue and send our FIN, advancing toward the closing states. */
static void tcp_begin_close(struct inet_socket *s) {
    struct tcp_control_block *tcp = s->tcp;
    if (!tcp || tcp->fin_sent) return;
    if (tcp->state == TCP_ESTABLISHED || tcp->state == TCP_CLOSE_WAIT) {
        tcp_output(s);
        uint32_t seq = tcp->snd_una + (uint32_t)tcp->tx_sent;
        tcp_transmit(s, seq, TCP_ACK | TCP_FIN, NULL, 0);
        tcp->fin_sent = 1;
        tcp->snd_nxt = seq + 1U;
        tcp->state = (tcp->state == TCP_ESTABLISHED) ? TCP_FIN_WAIT_1 : TCP_LAST_ACK;
        tcp_arm_rto(tcp);
    } else if (tcp->state == TCP_SYN_SENT) {
        tcp->state = TCP_CLOSED;
        tcp->rto_deadline_ns = 0;
    }
}

static int tcp_connect(struct inet_socket *socket, uint32_t address, uint16_t port) {
    struct tcp_control_block *tcp = socket->tcp;
    if (tcp) {
        /* Idempotent: re-entered by the blocking connect retry loop. */
        net_poll();
        if (tcp->pending_error) { int e = tcp->pending_error; tcp->pending_error = 0; return e; }
        if (tcp->state == TCP_ESTABLISHED || tcp->state >= TCP_FIN_WAIT_1) {
            if (!socket->connected) { socket->connected = 1; return 0; }
            return -EISCONN;
        }
        return -EINPROGRESS;
    }
    if (!address || !port) return -EINVAL;
    const struct net_config *config = net_get_config();
    if (!config->link_up || !config->interface_up) return -ENETDOWN;
    tcp = (struct tcp_control_block *)kmalloc(sizeof(*tcp));
    if (!tcp) return -ENOMEM;
    memset(tcp, 0, sizeof(*tcp));
    socket->tcp = tcp;
    if (!socket->local_port) socket->local_port = allocate_port();
    if (!socket->local_port) { socket->tcp = NULL; kfree(tcp); return -EADDRINUSE; }
    if (!socket->local_address) socket->local_address = config->address;
    socket->peer_address = address;
    socket->peer_port = port;
    tcp->iss = tcp_generate_iss();
    tcp->snd_una = tcp->iss;
    tcp->snd_nxt = tcp->iss + 1U;   /* SYN consumes one sequence number */
    tcp->state = TCP_SYN_SENT;
    tcp_transmit(socket, tcp->iss, TCP_SYN, NULL, 0);
    tcp_arm_rto(tcp);
    return -EINPROGRESS;
}

static void tcp_enter_time_wait(struct inet_socket *s) {
    struct tcp_control_block *tcp = s->tcp;
    tcp->state = TCP_TIME_WAIT;
    tcp->rto_deadline_ns = 0;
    tcp->time_wait_deadline_ns = time_uptime_ns() + TCP_TIME_WAIT_NS;
}

static void tcp_process_ack(struct inet_socket *s, uint32_t ack) {
    struct tcp_control_block *tcp = s->tcp;
    if (!seq_gt(ack, tcp->snd_una) || !seq_le(ack, tcp->snd_nxt)) return;
    uint32_t acked = ack - tcp->snd_una;
    uint32_t data_acked = acked;
    if (tcp->fin_sent && ack == tcp->snd_nxt) {
        tcp->fin_acked = 1;
        if (data_acked > 0) data_acked -= 1U;   /* FIN occupies one sequence */
    }
    if (data_acked > tcp->tx_sent) data_acked = (uint32_t)tcp->tx_sent;
    if (data_acked > 0) {
        memmove(tcp->tx, tcp->tx + data_acked, tcp->tx_len - data_acked);
        tcp->tx_len -= data_acked;
        tcp->tx_sent -= data_acked;
    }
    tcp->snd_una = ack;
    tcp->retransmit_count = 0;
    tcp->rto_ns = TCP_RTO_INIT_NS;
    if (tcp->snd_una == tcp->snd_nxt) tcp->rto_deadline_ns = 0;
    else tcp_arm_rto(tcp);
}

/* Advance close-handshake states after processing an incoming segment. */
static void tcp_advance_close(struct inet_socket *s) {
    struct tcp_control_block *tcp = s->tcp;
    switch (tcp->state) {
        case TCP_ESTABLISHED:
            if (tcp->peer_fin) tcp->state = TCP_CLOSE_WAIT;
            break;
        case TCP_FIN_WAIT_1:
            if (tcp->fin_acked && tcp->peer_fin) tcp_enter_time_wait(s);
            else if (tcp->fin_acked) tcp->state = TCP_FIN_WAIT_2;
            else if (tcp->peer_fin) tcp->state = TCP_CLOSING;
            break;
        case TCP_FIN_WAIT_2:
            if (tcp->peer_fin) tcp_enter_time_wait(s);
            break;
        case TCP_CLOSING:
            if (tcp->fin_acked) tcp_enter_time_wait(s);
            break;
        case TCP_LAST_ACK:
            if (tcp->fin_acked) { tcp->state = TCP_CLOSED; tcp->rto_deadline_ns = 0; }
            break;
        default: break;
    }
}

static void tcp_input(struct inet_socket *s, uint32_t seq, uint32_t ack, uint8_t flags,
                      uint16_t window, const uint8_t *payload, size_t length) {
    struct tcp_control_block *tcp = s->tcp;
    if (!tcp) return;

    if (flags & TCP_RST) {
        tcp->pending_error = (tcp->state == TCP_SYN_SENT) ? -ECONNREFUSED : -ECONNRESET;
        tcp->state = TCP_CLOSED;
        tcp->rto_deadline_ns = 0;
        tcp->peer_fin = 1;
        return;
    }

    tcp->snd_wnd = window;

    if (tcp->state == TCP_SYN_SENT) {
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            if (ack != tcp->iss + 1U) return;   /* stale/invalid, ignore */
            tcp->irs = seq;
            tcp->rcv_nxt = seq + 1U;
            tcp->snd_una = ack;
            tcp->snd_nxt = ack;
            tcp->state = TCP_ESTABLISHED;
            tcp->rto_deadline_ns = 0;
            tcp_send_ack(s);
        }
        return;
    }

    if (flags & TCP_ACK) tcp_process_ack(s, ack);

    if (length > 0 && seq == tcp->rcv_nxt && !tcp->peer_fin) {
        size_t space = TCP_RING - tcp->rx_len;
        size_t take = length < space ? length : space;
        for (size_t i = 0; i < take; i++)
            tcp->rx[(tcp->rx_head + tcp->rx_len + i) % TCP_RING] = payload[i];
        tcp->rx_len += take;
        tcp->rcv_nxt += (uint32_t)take;
        tcp_send_ack(s);
    } else if (length > 0) {
        /* Out-of-order or already-received data: re-acknowledge. */
        tcp_send_ack(s);
    }

    if ((flags & TCP_FIN) && seq + (uint32_t)length == tcp->rcv_nxt && !tcp->peer_fin) {
        tcp->rcv_nxt += 1U;
        tcp->peer_fin = 1;
        tcp_send_ack(s);
    }

    /* A fresh ACK may have opened the send window; flush any buffered data. */
    tcp_output(s);
    tcp_advance_close(s);
}

static int64_t tcp_send(struct inet_socket *s, const void *data, size_t length) {
    struct tcp_control_block *tcp = s->tcp;
    if (!tcp) return -ENOTCONN;
    if (tcp->pending_error) { int e = tcp->pending_error; tcp->pending_error = 0; return e; }
    if (s->write_shutdown || tcp->fin_sent) return -EPIPE;
    if (tcp->state == TCP_SYN_SENT) return -EAGAIN;   /* still connecting */
    if (tcp->state != TCP_ESTABLISHED && tcp->state != TCP_CLOSE_WAIT) return -ENOTCONN;
    if (!length) return 0;
    size_t space = TCP_RING - tcp->tx_len;
    if (!space) { net_poll(); return -EAGAIN; }
    size_t take = length < space ? length : space;
    memcpy(tcp->tx + tcp->tx_len, data, take);
    tcp->tx_len += take;
    tcp_output(s);
    return (int64_t)take;
}

static int64_t tcp_recv(struct inet_socket *s, void *data, size_t length, int flags) {
    struct tcp_control_block *tcp = s->tcp;
    if (!tcp) return -ENOTCONN;
    net_poll();
    if (tcp->rx_len == 0) {
        if (tcp->pending_error) { int e = tcp->pending_error; tcp->pending_error = 0; return e; }
        if (tcp->peer_fin || s->read_shutdown || tcp->state == TCP_CLOSED) return 0;
        return -EAGAIN;
    }
    if (!length) return 0;
    size_t take = length < tcp->rx_len ? length : tcp->rx_len;
    uint8_t *out = (uint8_t *)data;
    for (size_t i = 0; i < take; i++)
        out[i] = tcp->rx[(tcp->rx_head + i) % TCP_RING];
    if (!(flags & MSG_PEEK)) {
        tcp->rx_head = (tcp->rx_head + take) % TCP_RING;
        tcp->rx_len -= take;
    }
    return (int64_t)take;
}

static void tcp_retransmit(struct inet_socket *s) {
    struct tcp_control_block *tcp = s->tcp;
    if (++tcp->retransmit_count > TCP_MAX_RETRIES) {
        tcp->pending_error = -ETIMEDOUT;
        tcp->state = TCP_CLOSED;
        tcp->rto_deadline_ns = 0;
        tcp->peer_fin = 1;
        return;
    }
    tcp->rto_ns = tcp->rto_ns ? tcp->rto_ns * 2U : TCP_RTO_INIT_NS;
    if (tcp->rto_ns > TCP_RTO_MAX_NS) tcp->rto_ns = TCP_RTO_MAX_NS;
    switch (tcp->state) {
        case TCP_SYN_SENT:
            tcp_transmit(s, tcp->iss, TCP_SYN, NULL, 0);
            break;
        case TCP_ESTABLISHED:
        case TCP_CLOSE_WAIT:
        case TCP_FIN_WAIT_1:
        case TCP_CLOSING:
        case TCP_LAST_ACK:
            if (tcp->tx_sent > 0) {
                size_t chunk = tcp->tx_sent < TCP_MSS ? tcp->tx_sent : TCP_MSS;
                tcp_transmit(s, tcp->snd_una, TCP_ACK | TCP_PSH, tcp->tx, chunk);
            } else if (tcp->fin_sent) {
                tcp_transmit(s, tcp->snd_nxt - 1U, TCP_ACK | TCP_FIN, NULL, 0);
            }
            break;
        default: break;
    }
    tcp->rto_deadline_ns = time_uptime_ns() + tcp->rto_ns;
}

void inet_socket_tcp_timer_poll(void) {
    uint64_t now = time_uptime_ns();
    for (unsigned i = 0; i < MAX_INET_SOCKETS; i++) {
        struct inet_socket *s = sockets[i];
        if (!s || !s->tcp) continue;
        struct tcp_control_block *tcp = s->tcp;
        if (tcp->state == TCP_TIME_WAIT && now >= tcp->time_wait_deadline_ns) {
            tcp->state = TCP_CLOSED;
            tcp->rto_deadline_ns = 0;
        }
        if (tcp->rto_deadline_ns && now >= tcp->rto_deadline_ns) tcp_retransmit(s);
        if (s->orphan && (tcp->state == TCP_CLOSED || now >= tcp->orphan_deadline_ns))
            tcp_free(s);
    }
}

void inet_socket_receive_tcp(uint32_t source, uint16_t source_port, uint32_t destination,
                             uint16_t destination_port, uint32_t seq, uint32_t ack, uint8_t flags,
                             uint16_t window, const uint8_t *payload, size_t length) {
    for (unsigned i = 0; i < MAX_INET_SOCKETS; i++) {
        struct inet_socket *s = sockets[i];
        if (!s || !s->tcp || s->type != TUNIX_SOCK_STREAM) continue;
        if (s->local_port != destination_port) continue;
        if (s->local_address && s->local_address != destination) continue;
        if (s->peer_port != source_port || s->peer_address != source) continue;
        tcp_input(s, seq, ack, flags, window, payload, length);
        return;
    }
    /* Unmatched segment: reset the peer so it stops retransmitting. */
    if (!(flags & TCP_RST)) {
        uint32_t rst_seq = (flags & TCP_ACK) ? ack : 0U;
        uint32_t rst_ack = seq + (uint32_t)length + ((flags & (TCP_SYN | TCP_FIN)) ? 1U : 0U);
        uint8_t rst_flags = (flags & TCP_ACK) ? TCP_RST : (TCP_RST | TCP_ACK);
        net_send_tcp(destination, destination_port, source, source_port,
                     rst_seq, rst_ack, rst_flags, 0, NULL, 0);
    }
}

struct inet_socket *inet_socket_create(int domain, int type, int protocol) {
    int base_type = type & 0xFU;
    if (domain == TUNIX_AF_INET) {
        if (base_type == TUNIX_SOCK_STREAM) {
            if (protocol != 0 && protocol != 6) return NULL;
        } else if (base_type == TUNIX_SOCK_DGRAM) {
            if (protocol != 0 && protocol != 17) return NULL;
        } else if (base_type != TUNIX_SOCK_RAW) return NULL;
    } else if (domain == TUNIX_AF_PACKET) {
        if (base_type != TUNIX_SOCK_DGRAM && base_type != TUNIX_SOCK_RAW &&
            base_type != TUNIX_SOCK_PACKET) return NULL;
    } else return NULL;
    struct inet_socket *socket = (struct inet_socket *)kmalloc(sizeof(*socket));
    if (!socket) return NULL;
    memset(socket, 0, sizeof(*socket));
    socket->refs = 1;
    socket->domain = domain;
    socket->type = base_type;
    socket->protocol = protocol;
    socket->ttl = 64;
    if (register_socket(socket) != 0) { kfree(socket); return NULL; }
    return socket;
}

void inet_socket_ref(struct inet_socket *socket) { if (socket) socket->refs++; }
void inet_socket_unref(struct inet_socket *socket) {
    if (!socket || --socket->refs > 0) return;
    /* A TCP connection that is still open must finish its close handshake even
       though the last fd is gone. Send a FIN and keep the (orphaned) TCB in the
       socket table so the timer sweep can free it once it reaches CLOSED. */
    if (socket->tcp && !socket->orphan && socket->tcp->state != TCP_CLOSED) {
        tcp_begin_close(socket);
        if (socket->tcp->state != TCP_CLOSED) {
            socket->orphan = 1;
            socket->tcp->orphan_deadline_ns = time_uptime_ns() + TCP_ORPHAN_NS;
            return;
        }
    }
    tcp_free(socket);
}

static int local_port_conflict(struct inet_socket *socket, uint32_t address, uint16_t port) {
    for (unsigned i = 0; i < MAX_INET_SOCKETS; i++) {
        struct inet_socket *other = sockets[i];
        if (!other || other == socket || other->domain != TUNIX_AF_INET ||
            other->type != socket->type || other->local_port != port) continue;
        if (!other->local_address || !address || other->local_address == address) return 1;
    }
    return 0;
}

int inet_socket_bind(struct inet_socket *socket, const void *address, size_t length) {
    if (!socket || !address) return -EINVAL;
    if (socket->domain == TUNIX_AF_INET) {
        if (length < sizeof(struct tunix_sockaddr_in)) return -EINVAL;
        const struct tunix_sockaddr_in *in = (const struct tunix_sockaddr_in *)address;
        if (in->family != TUNIX_AF_INET) return -EAFNOSUPPORT;
        uint16_t port = net_htons(in->port);
        if (port && local_port_conflict(socket, in->address, port)) return -EADDRINUSE;
        socket->local_address = in->address;
        socket->local_port = port;
        socket->bound = 1;
        return 0;
    }
    if (socket->domain == TUNIX_AF_PACKET) {
        if (length < sizeof(struct tunix_sockaddr_ll)) return -EINVAL;
        const struct tunix_sockaddr_ll *ll = (const struct tunix_sockaddr_ll *)address;
        if (ll->family != TUNIX_AF_PACKET || (ll->ifindex != 0 && ll->ifindex != 1))
            return -EADDRNOTAVAIL;
        if (ll->protocol) socket->protocol = ll->protocol;
        socket->bound = 1;
        return 0;
    }
    return -EAFNOSUPPORT;
}

int inet_socket_connect(struct inet_socket *socket, const void *address, size_t length) {
    if (!socket || socket->domain != TUNIX_AF_INET || !address ||
        length < sizeof(struct tunix_sockaddr_in)) return -EINVAL;
    const struct tunix_sockaddr_in *in = (const struct tunix_sockaddr_in *)address;
    if (in->family != TUNIX_AF_INET) return -EAFNOSUPPORT;
    if (socket->type == TUNIX_SOCK_STREAM)
        return tcp_connect(socket, in->address, net_htons(in->port));
    if (!socket->local_port) socket->local_port = allocate_port();
    if (!socket->local_port) return -EADDRINUSE;
    socket->peer_address = in->address;
    socket->peer_port = net_htons(in->port);
    socket->connected = 1;
    return 0;
}

static int enqueue(struct inet_socket *socket, const void *data, size_t length,
                   const void *address, size_t address_length) {
    if (!socket || socket->queue_count >= SOCKET_QUEUE) return -EAGAIN;
    if (length > SOCKET_PACKET_MAX) length = SOCKET_PACKET_MAX;
    struct queued_packet *item = &socket->queue[socket->queue_tail];
    item->length = length;
    memcpy(item->data, data, length);
    item->address_length = address_length > sizeof(item->address) ? sizeof(item->address) : address_length;
    if (address && item->address_length) memcpy(item->address, address, item->address_length);
    socket->queue_tail = (socket->queue_tail + 1U) % SOCKET_QUEUE;
    socket->queue_count++;
    return 0;
}

int64_t inet_socket_sendto(struct inet_socket *socket, const void *data, size_t length, int flags,
                           const void *address, size_t address_length) {
    (void)flags;
    if (!socket || !data) return -EINVAL;
    if (socket->type == TUNIX_SOCK_STREAM) return tcp_send(socket, data, length);
    if (socket->write_shutdown) return -EPIPE;
    const struct net_config *config = net_get_config();
    if (!config->link_up || !config->interface_up) return -ENETDOWN;
    if (socket->domain == TUNIX_AF_INET) {
        uint32_t destination = socket->peer_address;
        uint16_t port = socket->peer_port;
        if (address) {
            if (address_length < sizeof(struct tunix_sockaddr_in)) return -EINVAL;
            const struct tunix_sockaddr_in *in = (const struct tunix_sockaddr_in *)address;
            if (in->family != TUNIX_AF_INET) return -EAFNOSUPPORT;
            destination = in->address;
            port = net_htons(in->port);
        }
        if (!destination) return -EDESTADDRREQ;
        if (!socket->local_port) socket->local_port = allocate_port();
        if (socket->type == TUNIX_SOCK_DGRAM) {
            if (!port) return -EDESTADDRREQ;
            if (net_send_udp(socket->local_address, socket->local_port, destination, port,
                             data, length) != 0) return -EAGAIN;
        } else {
            if (net_send_ipv4(destination, (uint8_t)socket->protocol, data, length,
                              socket->ttl, socket->header_included) != 0) return -EAGAIN;
        }
        return (int64_t)length;
    }
    if (socket->domain == TUNIX_AF_PACKET) {
        if (socket->type == TUNIX_SOCK_PACKET || socket->type == TUNIX_SOCK_RAW) {
            return net_send_raw_ethernet(data, length) == 0 ? (int64_t)length : -EAGAIN;
        }
        if (!address || address_length < sizeof(struct tunix_sockaddr_ll)) return -EDESTADDRREQ;
        const struct tunix_sockaddr_ll *ll = (const struct tunix_sockaddr_ll *)address;
        uint16_t type = net_htons(ll->protocol ? ll->protocol : (uint16_t)socket->protocol);
        return net_send_ethernet(ll->address, type, data, length) == 0 ? (int64_t)length : -EAGAIN;
    }
    return -EAFNOSUPPORT;
}

int64_t inet_socket_recvfrom(struct inet_socket *socket, void *data, size_t length, int flags,
                             void *address, size_t *address_length) {
    if (!socket || !data) return -EINVAL;
    if (socket->type == TUNIX_SOCK_STREAM) return tcp_recv(socket, data, length, flags);
    if (socket->read_shutdown) return 0;
    net_poll();
    if (!socket->queue_count) return -EAGAIN;
    struct queued_packet *item = &socket->queue[socket->queue_head];
    size_t amount = length < item->length ? length : item->length;
    memcpy(data, item->data, amount);
    if (address && address_length) {
        size_t copy = *address_length < item->address_length ? *address_length : item->address_length;
        memcpy(address, item->address, copy);
        *address_length = item->address_length;
    }
    if (!(flags & MSG_PEEK)) {
        socket->queue_head = (socket->queue_head + 1U) % SOCKET_QUEUE;
        socket->queue_count--;
    }
    return (int64_t)amount;
}

int inet_socket_getsockname(struct inet_socket *socket, void *address, size_t *length) {
    if (!socket || !address || !length) return -EINVAL;
    if (socket->domain != TUNIX_AF_INET || *length < sizeof(struct tunix_sockaddr_in)) return -EINVAL;
    struct tunix_sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.family = TUNIX_AF_INET;
    in.port = net_htons(socket->local_port);
    in.address = socket->local_address;
    memcpy(address, &in, sizeof(in));
    *length = sizeof(in);
    return 0;
}

int inet_socket_getpeername(struct inet_socket *socket, void *address, size_t *length) {
    if (!socket || !socket->connected) return -ENOTCONN;
    if (!address || !length || *length < sizeof(struct tunix_sockaddr_in)) return -EINVAL;
    struct tunix_sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.family = TUNIX_AF_INET;
    in.port = net_htons(socket->peer_port);
    in.address = socket->peer_address;
    memcpy(address, &in, sizeof(in));
    *length = sizeof(in);
    return 0;
}

int inet_socket_setsockopt(struct inet_socket *socket, int level, int option,
                           const void *value, size_t length) {
    if (!socket) return -EINVAL;
    if (level == SOL_SOCKET) {
        if (option == SO_BROADCAST && value && length >= sizeof(int)) {
            socket->broadcast = *(const int *)value != 0; return 0;
        }
        if (option == SO_BINDTODEVICE || option == SO_ATTACH_FILTER || option == SO_RCVBUF ||
            option == SO_SNDBUF || option == SO_RCVTIMEO || option == SO_SNDTIMEO) return 0;
        return 0;
    }
    if (level == IPPROTO_IP) {
        if (option == IP_HDRINCL && value && length >= sizeof(int)) {
            socket->header_included = *(const int *)value != 0; return 0;
        }
        if (option == IP_TTL && value && length >= sizeof(int)) {
            int ttl = *(const int *)value;
            if (ttl < 1 || ttl > 255) return -EINVAL;
            socket->ttl = (uint8_t)ttl; return 0;
        }
    }
    return -EOPNOTSUPP;
}

int inet_socket_getsockopt(struct inet_socket *socket, int level, int option,
                           void *value, size_t *length) {
    if (!socket || !value || !length || *length < sizeof(int)) return -EINVAL;
    int result = 0;
    if (level == SOL_SOCKET && option == SO_ERROR) {
        /* Report and clear a pending connection error (e.g. -ECONNREFUSED),
           the way a non-blocking connect() result is collected. */
        if (socket->tcp && socket->tcp->pending_error) {
            result = -socket->tcp->pending_error;
            socket->tcp->pending_error = 0;
        }
    }
    else if (level == SOL_SOCKET && option == SO_BROADCAST) result = socket->broadcast;
    else if (level == IPPROTO_IP && option == IP_TTL) result = socket->ttl;
    else return -EOPNOTSUPP;
    memcpy(value, &result, sizeof(result));
    *length = sizeof(result);
    return 0;
}

static int ifname_valid(const uint8_t *argument) {
    return (!argument[0]) || (argument[0] == 'e' && argument[1] == 't' &&
        argument[2] == 'h' && argument[3] == '0' && argument[4] == 0);
}

static void set_sockaddr(uint8_t *where, uint32_t address) {
    memset(where, 0, 16);
    where[0] = TUNIX_AF_INET;
    memcpy(where + 4, &address, 4);
}

int inet_socket_ioctl(struct inet_socket *socket, unsigned long request, void *argument) {
    if (!socket || !argument) return -EINVAL;
    uint8_t *arg = (uint8_t *)argument;
    const struct net_config *cfg = net_get_config();
    if (request == SIOCADDRT || request == SIOCDELRT) {
        if (request == SIOCADDRT) {
            uint32_t gateway;
            memcpy(&gateway, arg + 20, 4);
            if (gateway) net_set_gateway(gateway);
        }
        return 0;
    }
    if (!ifname_valid(arg)) return -EADDRNOTAVAIL;
    if (!arg[0]) { arg[0]='e'; arg[1]='t'; arg[2]='h'; arg[3]='0'; arg[4]=0; }
    switch (request) {
        case SIOCGIFFLAGS: {
            int16_t flags = IFF_BROADCAST | IFF_MULTICAST;
            if (cfg->interface_up) flags |= IFF_UP;
            if (cfg->link_up) flags |= IFF_RUNNING;
            memcpy(arg + 16, &flags, sizeof(flags)); return 0;
        }
        case SIOCSIFFLAGS: {
            int16_t flags; memcpy(&flags, arg + 16, sizeof(flags));
            net_set_interface_up((flags & IFF_UP) != 0); return 0;
        }
        case SIOCGIFADDR: set_sockaddr(arg + 16, cfg->address); return 0;
        case SIOCSIFADDR: { uint32_t value; memcpy(&value, arg + 20, 4); net_set_address(value); return 0; }
        case SIOCGIFNETMASK: set_sockaddr(arg + 16, cfg->netmask); return 0;
        case SIOCSIFNETMASK: { uint32_t value; memcpy(&value, arg + 20, 4); net_set_netmask(value); return 0; }
        case SIOCGIFBRDADDR: set_sockaddr(arg + 16, cfg->address | ~cfg->netmask); return 0;
        case SIOCGIFHWADDR:
            memset(arg + 16, 0, 16); arg[16] = 1; memcpy(arg + 18, cfg->mac, 6); return 0;
        case SIOCGIFINDEX: { int index = 1; memcpy(arg + 16, &index, 4); return 0; }
        case SIOCGIFMTU: { int mtu = 1500; memcpy(arg + 16, &mtu, 4); return 0; }
        default: return -ENOTTY;
    }
}

int inet_socket_read_ready(struct inet_socket *socket) {
    net_poll();
    if (!socket) return 0;
    if (socket->tcp) {
        struct tcp_control_block *tcp = socket->tcp;
        return tcp->rx_len > 0 || tcp->peer_fin || tcp->pending_error ||
               tcp->state == TCP_CLOSED || socket->read_shutdown;
    }
    return socket->read_shutdown || socket->queue_count;
}
int inet_socket_write_ready(struct inet_socket *socket) {
    const struct net_config *cfg = net_get_config();
    if (!socket) return 0;
    if (socket->tcp) {
        struct tcp_control_block *tcp = socket->tcp;
        if (tcp->state == TCP_ESTABLISHED || tcp->state == TCP_CLOSE_WAIT)
            return tcp->tx_len < TCP_RING;
        /* Report a failed/closed connection as writable so poll() unblocks. */
        return tcp->state == TCP_CLOSED || tcp->pending_error != 0;
    }
    return !socket->write_shutdown && cfg->link_up && cfg->interface_up;
}
int inet_socket_peer_closed(struct inet_socket *socket) {
    return socket && socket->tcp && (socket->tcp->peer_fin || socket->tcp->pending_error);
}
int inet_socket_shutdown(struct inet_socket *socket, int how) {
    if (!socket) return -EINVAL;
    if (how < 0 || how > 2) return -EINVAL;
    if (socket->tcp) {
        if (how == 0 || how == 2) socket->read_shutdown = 1;
        if (how == 1 || how == 2) tcp_begin_close(socket);
        return 0;
    }
    if (!socket->connected) return -ENOTCONN;
    if (how == 0 || how == 2) {
        socket->read_shutdown = 1;
        socket->queue_head = 0;
        socket->queue_tail = 0;
        socket->queue_count = 0;
    }
    if (how == 1 || how == 2) socket->write_shutdown = 1;
    return 0;
}
int64_t inet_socket_read(struct inet_socket *socket, size_t length, void *data) {
    return inet_socket_recvfrom(socket, data, length, 0, NULL, NULL);
}
int64_t inet_socket_write(struct inet_socket *socket, size_t length, const void *data) {
    return inet_socket_sendto(socket, data, length, 0, NULL, 0);
}

void inet_socket_receive_udp(const uint8_t *payload, size_t length, uint32_t source,
                             uint16_t source_port, uint32_t destination, uint16_t destination_port) {
    struct tunix_sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.family = TUNIX_AF_INET;
    address.port = net_htons(source_port);
    address.address = source;
    for (unsigned i = 0; i < MAX_INET_SOCKETS; i++) {
        struct inet_socket *socket = sockets[i];
        if (!socket || socket->domain != TUNIX_AF_INET || socket->type != TUNIX_SOCK_DGRAM) continue;
        if (socket->local_port != destination_port) continue;
        if (socket->local_address && socket->local_address != destination) continue;
        if (socket->connected && (socket->peer_address != source || socket->peer_port != source_port)) continue;
        (void)enqueue(socket, payload, length, &address, sizeof(address));
    }
}

void inet_socket_receive_ipv4(const uint8_t *packet, size_t length, uint8_t protocol,
                              uint32_t source, uint32_t destination) {
    (void)destination;
    struct tunix_sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.family = TUNIX_AF_INET;
    address.address = source;
    for (unsigned i = 0; i < MAX_INET_SOCKETS; i++) {
        struct inet_socket *socket = sockets[i];
        if (!socket || socket->domain != TUNIX_AF_INET || socket->type != TUNIX_SOCK_RAW) continue;
        if (socket->protocol && socket->protocol != protocol) continue;
        (void)enqueue(socket, packet, length, &address, sizeof(address));
    }
}

void inet_socket_receive_ethernet(const uint8_t *frame, size_t length, uint16_t ethertype) {
    if (length < 14) return;
    struct tunix_sockaddr_ll address;
    memset(&address, 0, sizeof(address));
    address.family = TUNIX_AF_PACKET;
    address.protocol = net_htons(ethertype);
    address.ifindex = 1;
    address.hatype = 1;
    address.halen = 6;
    memcpy(address.address, frame + 6, 6);
    for (unsigned i = 0; i < MAX_INET_SOCKETS; i++) {
        struct inet_socket *socket = sockets[i];
        if (!socket || socket->domain != TUNIX_AF_PACKET) continue;
        uint16_t filter = net_htons((uint16_t)socket->protocol);
        if (filter && filter != 3U && filter != ethertype) continue;
        if (socket->type == TUNIX_SOCK_DGRAM)
            (void)enqueue(socket, frame + 14, length - 14, &address, sizeof(address));
        else
            (void)enqueue(socket, frame, length, &address, sizeof(address));
    }
}

static void text_char(char *buffer, size_t capacity, size_t *length, char value) {
    if (*length + 1 < capacity) buffer[(*length)++] = value;
}
static void text_string(char *buffer, size_t capacity, size_t *length, const char *value) {
    while (*value) text_char(buffer, capacity, length, *value++);
}
static void text_hex4(char *buffer, size_t capacity, size_t *length, uint16_t value) {
    static const char digits[] = "0123456789ABCDEF";
    for (int shift = 12; shift >= 0; shift -= 4) text_char(buffer, capacity, length, digits[(value >> shift) & 15]);
}
static void text_hex8(char *buffer, size_t capacity, size_t *length, uint32_t value) {
    text_hex4(buffer, capacity, length, (uint16_t)(value >> 16));
    text_hex4(buffer, capacity, length, (uint16_t)value);
}

void inet_socket_proc_udp(char *buffer, size_t capacity, size_t *length) {
    text_string(buffer, capacity, length, "  sl  local_address rem_address   st\n");
    unsigned slot = 0;
    for (unsigned i = 0; i < MAX_INET_SOCKETS; i++) {
        struct inet_socket *s = sockets[i];
        if (!s || s->domain != TUNIX_AF_INET || s->type != TUNIX_SOCK_DGRAM) continue;
        text_char(buffer, capacity, length, ' '); text_hex4(buffer, capacity, length, (uint16_t)slot++);
        text_string(buffer, capacity, length, ": "); text_hex8(buffer, capacity, length, s->local_address);
        text_char(buffer, capacity, length, ':'); text_hex4(buffer, capacity, length, s->local_port);
        text_char(buffer, capacity, length, ' '); text_hex8(buffer, capacity, length, s->peer_address);
        text_char(buffer, capacity, length, ':'); text_hex4(buffer, capacity, length, s->peer_port);
        text_string(buffer, capacity, length, " 07\n");
    }
}
void inet_socket_proc_raw(char *buffer, size_t capacity, size_t *length) {
    text_string(buffer, capacity, length, "  sl  local_address rem_address   st\n");
    unsigned slot = 0;
    for (unsigned i = 0; i < MAX_INET_SOCKETS; i++) {
        struct inet_socket *s = sockets[i];
        if (!s || s->domain != TUNIX_AF_INET || s->type != TUNIX_SOCK_RAW) continue;
        text_char(buffer, capacity, length, ' '); text_hex4(buffer, capacity, length, (uint16_t)slot++);
        text_string(buffer, capacity, length, ": "); text_hex8(buffer, capacity, length, s->local_address);
        text_string(buffer, capacity, length, ":0000 00000000:0000 07\n");
    }
}
void inet_socket_proc_tcp(char *buffer, size_t capacity, size_t *length) {
    /* Map our enum tcp_state to the Linux /proc/net/tcp state codes. */
    static const char *const codes[] = {
        "07", "02", "01", "04", "05", "0B", "06", "08", "09"
    };
    text_string(buffer, capacity, length, "  sl  local_address rem_address   st\n");
    unsigned slot = 0;
    for (unsigned i = 0; i < MAX_INET_SOCKETS; i++) {
        struct inet_socket *s = sockets[i];
        if (!s || !s->tcp || s->type != TUNIX_SOCK_STREAM) continue;
        text_char(buffer, capacity, length, ' '); text_hex4(buffer, capacity, length, (uint16_t)slot++);
        text_string(buffer, capacity, length, ": "); text_hex8(buffer, capacity, length, s->local_address);
        text_char(buffer, capacity, length, ':'); text_hex4(buffer, capacity, length, s->local_port);
        text_char(buffer, capacity, length, ' '); text_hex8(buffer, capacity, length, s->peer_address);
        text_char(buffer, capacity, length, ':'); text_hex4(buffer, capacity, length, s->peer_port);
        text_char(buffer, capacity, length, ' ');
        int state = s->tcp->state;
        text_string(buffer, capacity, length, (state >= 0 && state <= 8) ? codes[state] : "07");
        text_char(buffer, capacity, length, '\n');
    }
}
