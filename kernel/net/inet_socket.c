#include <stddef.h>
#include <stdint.h>
#include "../include/heap.h"
#include "../include/kstring.h"
#include "../include/time.h"
#include "../include/net/inet_socket.h"
#include "../include/eventfs.h"
#include "../include/net/net.h"

extern void kprintf(const char *fmt, ...);

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
#define ENODEV 19
#define ENETDOWN 100
#define ENOTCONN 107
#define EPIPE 32
#define EMSGSIZE 90
#define ECONNRESET 104
#define EISCONN 106
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EINPROGRESS 115

#define DATAGRAM_BUFFER_DEFAULT 212992U
#define STREAM_BUFFER_DEFAULT (256U << 10)
#define SOCKET_BUFFER_MIN 2048U
#define SOCKET_BUFFER_MAX (16U << 20)
#define TCP_BUFFER_INITIAL 16384U
#define TCP_DEFAULT_MSS 536U
#define TCP_MAX_WINDOW_SCALE 14U
#define TCP_DUP_ACK_LIMIT 3U

#define TCP_RTO_INIT_NS   200000000ULL
#define TCP_RTO_MAX_NS   4000000000ULL
#define TCP_MAX_RETRIES  8
#define TCP_TIME_WAIT_NS 10000000000ULL
#define TCP_ORPHAN_NS    30000000000ULL

#define TCP_BACKLOG_MAX 4096

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
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
    uint32_t iss;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t snd_wnd;
    uint32_t irs;
    uint32_t rcv_nxt;
    int fin_queued;
    int fin_sent;
    int fin_acked;
    int peer_fin;
    int pending_error;
    uint8_t *tx;
    size_t tx_capacity;
    size_t tx_len;
    size_t tx_sent;
    uint8_t *rx;
    size_t rx_capacity;
    size_t rx_head;
    size_t rx_len;
    uint32_t rcv_wnd_adv;
    uint32_t send_mss;
    uint8_t snd_wscale;
    uint8_t rcv_wscale;
    uint8_t scaling;
    unsigned dup_acks;
    uint64_t rto_ns;
    uint64_t rto_deadline_ns;
    unsigned retransmit_count;
    uint64_t time_wait_deadline_ns;
    uint64_t orphan_deadline_ns;
};

#define MSG_PEEK 0x02
#define MSG_DONTWAIT 0x40
#define SOL_SOCKET 1
#define SOL_PACKET 263
#define SO_ERROR 4
#define SO_BROADCAST 6
#define SO_RCVBUF 8
#define SO_SNDBUF 7
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_BINDTODEVICE 25
#define SO_ATTACH_FILTER 26
#define PACKET_AUXDATA 8
#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define SOL_RAW 255
#define ICMP_FILTER 1
#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO 8
#define IP_HDRINCL 3
#define IP_TTL 2
#define IP_RETOPTS 7
#define IP_PKTINFO 8
#define IP_RECVERR 11
#define IP_RECVTTL 12
#define IP_MTU_DISCOVER 10
#define IP_RECVTOS 13
#define SOL_UDP 17
#define UDP_GRO 104
#define TCP_NODELAY 1
#define TCP_KEEPIDLE 4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT 6

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
#define SIOCGIFNAME 0x8910U
#define SIOCGIFTXQLEN 0x8942U

#define IFF_UP 0x0001
#define IFF_BROADCAST 0x0002
#define IFF_LOOPBACK 0x0008
#define IFF_RUNNING 0x0040
#define IFF_MULTICAST 0x1000

struct datagram {
    struct datagram *next;
    size_t length;
    uint8_t address[32];
    size_t address_length;
    uint8_t ttl;
    uint8_t data[];
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
    int connect_reported;
    uint64_t event_pid;
    uint32_t event_uid;
    int event_open;
    int event_close;
    int bound;
    int read_shutdown;
    int write_shutdown;
    int broadcast;
    int header_included;

    int report_errors;
    uint32_t icmp_filter;
    int recv_ttl;
    int return_options;
    uint8_t last_ttl;
    uint8_t ttl;
    int orphan;
    size_t receive_buffer;
    size_t send_buffer;

    int listening;
    unsigned backlog;
    unsigned pending_total;
    struct inet_socket *pending;
    struct inet_socket *pending_last;
    struct inet_socket *listener;
    struct inet_socket *sibling;
    struct tcp_control_block *tcp;

    struct datagram *queue_first;
    struct datagram *queue_last;
    size_t queue_bytes;

    struct inet_socket *all_next;
    struct inet_socket *all_prev;
    struct inet_socket *class_next;
    struct inet_socket *class_prev;
    struct inet_socket *port_next;
    struct inet_socket *connection_next;
    struct inet_socket *timer_next;
    struct inet_socket *timer_prev;
    uint32_t port_key;
    uint32_t connection_key;
    uint8_t in_ports;
    uint8_t in_connections;
    uint8_t in_timers;
};

struct icmp_message {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
};

struct socket_table {
    struct inet_socket **buckets;
    uint32_t mask;
    uint32_t count;
};

static struct inet_socket *all_sockets;
static struct inet_socket *raw_sockets;
static struct inet_socket *packet_sockets;
static struct inet_socket *timed_sockets;
static struct inet_socket *timer_cursor;
static struct socket_table ports;
static struct socket_table connections;
static uint16_t next_ephemeral = 49152;

static size_t socket_total;

size_t inet_socket_count(void) {
    NET_LOCKED;
    return socket_total;
}

static int is_ping_socket(const struct inet_socket *socket) {
    return socket && socket->domain == TUNIX_AF_INET &&
           socket->type == TUNIX_SOCK_DGRAM && socket->protocol == IPPROTO_ICMP;
}

static struct inet_socket **class_list(const struct inet_socket *socket) {
    if (socket->domain == TUNIX_AF_PACKET) return &packet_sockets;
    if (socket->type == TUNIX_SOCK_RAW || is_ping_socket(socket)) return &raw_sockets;
    return NULL;
}

static uint32_t mix(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7FEB352DU;
    value ^= value >> 15;
    value *= 0x846CA68BU;
    return value ^ (value >> 16);
}

static uint32_t port_key_of(uint16_t port) {
    return mix(port);
}

static uint32_t connection_key_of(uint16_t local_port, uint32_t peer, uint16_t peer_port) {
    return mix(peer ^ mix(((uint32_t)local_port << 16) | peer_port));
}

static struct inet_socket **chain_next(struct inet_socket *socket, int by_connection) {
    return by_connection ? &socket->connection_next : &socket->port_next;
}

static uint32_t chain_key(const struct inet_socket *socket, int by_connection) {
    return by_connection ? socket->connection_key : socket->port_key;
}

static void table_link(struct socket_table *table, int by_connection,
                       struct inet_socket *socket) {
    struct inet_socket **bucket = &table->buckets[chain_key(socket, by_connection) & table->mask];
    *chain_next(socket, by_connection) = *bucket;
    *bucket = socket;
}

static int table_grow(struct socket_table *table, int by_connection) {
    uint32_t size = table->buckets ? (table->mask + 1U) * 2U : 64U;
    struct inet_socket **buckets =
        (struct inet_socket **)kmalloc(size * sizeof(*buckets));
    if (!buckets) return table->buckets ? 0 : -1;
    memset(buckets, 0, size * sizeof(*buckets));
    struct inet_socket **old = table->buckets;
    uint32_t old_size = old ? table->mask + 1U : 0;
    table->buckets = buckets;
    table->mask = size - 1U;
    for (uint32_t index = 0; index < old_size; index++) {
        struct inet_socket *socket = old[index];
        while (socket) {
            struct inet_socket *next = *chain_next(socket, by_connection);
            table_link(table, by_connection, socket);
            socket = next;
        }
    }
    kfree(old);
    return 0;
}

static int table_insert(struct socket_table *table, int by_connection,
                        struct inet_socket *socket) {
    if ((!table->buckets || table->count >= (table->mask + 1U) * 2U) &&
        table_grow(table, by_connection) != 0) return -1;
    table_link(table, by_connection, socket);
    table->count++;
    return 0;
}

static void table_remove(struct socket_table *table, int by_connection,
                         struct inet_socket *socket) {
    struct inet_socket **at = &table->buckets[chain_key(socket, by_connection) & table->mask];
    while (*at && *at != socket) at = chain_next(*at, by_connection);
    if (!*at) return;
    *at = *chain_next(socket, by_connection);
    *chain_next(socket, by_connection) = NULL;
    table->count--;
}

static struct inet_socket *port_bucket(uint16_t port) {
    if (!ports.buckets) return NULL;
    return ports.buckets[port_key_of(port) & ports.mask];
}

static void ports_leave(struct inet_socket *socket) {
    if (!socket->in_ports) return;
    table_remove(&ports, 0, socket);
    socket->in_ports = 0;
}

static int set_local_port(struct inet_socket *socket, uint16_t port) {
    ports_leave(socket);
    socket->local_port = port;
    if (!port || socket->domain != TUNIX_AF_INET || socket->listener) return 0;
    socket->port_key = port_key_of(port);
    if (table_insert(&ports, 0, socket) != 0) return -1;
    socket->in_ports = 1;
    return 0;
}

static int connections_enter(struct inet_socket *socket) {
    socket->connection_key = connection_key_of(socket->local_port, socket->peer_address,
                                               socket->peer_port);
    if (table_insert(&connections, 1, socket) != 0) return -1;
    socket->in_connections = 1;
    return 0;
}

static void connections_leave(struct inet_socket *socket) {
    if (!socket->in_connections) return;
    table_remove(&connections, 1, socket);
    socket->in_connections = 0;
}

static void timer_watch(struct inet_socket *socket) {
    if (socket->in_timers || !socket->tcp) return;
    socket->timer_prev = NULL;
    socket->timer_next = timed_sockets;
    if (timed_sockets) timed_sockets->timer_prev = socket;
    timed_sockets = socket;
    socket->in_timers = 1;
}

static void timer_unwatch(struct inet_socket *socket) {
    if (!socket->in_timers) return;
    if (timer_cursor == socket) timer_cursor = socket->timer_next;
    if (socket->timer_prev) socket->timer_prev->timer_next = socket->timer_next;
    else timed_sockets = socket->timer_next;
    if (socket->timer_next) socket->timer_next->timer_prev = socket->timer_prev;
    socket->timer_next = socket->timer_prev = NULL;
    socket->in_timers = 0;
}

static void register_socket(struct inet_socket *socket) {
    socket_total++;
    socket->all_prev = NULL;
    socket->all_next = all_sockets;
    if (all_sockets) all_sockets->all_prev = socket;
    all_sockets = socket;
    struct inet_socket **list = class_list(socket);
    if (!list) return;
    socket->class_prev = NULL;
    socket->class_next = *list;
    if (*list) (*list)->class_prev = socket;
    *list = socket;
}

static void unregister_socket(struct inet_socket *socket) {
    socket_total--;
    if (socket->all_prev) socket->all_prev->all_next = socket->all_next;
    else if (all_sockets == socket) all_sockets = socket->all_next;
    if (socket->all_next) socket->all_next->all_prev = socket->all_prev;
    struct inet_socket **list = class_list(socket);
    if (list) {
        if (socket->class_prev) socket->class_prev->class_next = socket->class_next;
        else if (*list == socket) *list = socket->class_next;
        if (socket->class_next) socket->class_next->class_prev = socket->class_prev;
    }
    ports_leave(socket);
    connections_leave(socket);
    timer_unwatch(socket);
}

static void queue_clear(struct inet_socket *socket) {
    struct datagram *item = socket->queue_first;
    while (item) {
        struct datagram *next = item->next;
        kfree(item);
        item = next;
    }
    socket->queue_first = socket->queue_last = NULL;
    socket->queue_bytes = 0;
}

static void destroy_socket(struct inet_socket *socket) {
    unregister_socket(socket);
    queue_clear(socket);
    if (socket->tcp) {
        kfree(socket->tcp->tx);
        kfree(socket->tcp->rx);
        kfree(socket->tcp);
    }
    kfree(socket);
}

static const char *event_protocol(const struct inet_socket *socket) {
    if (socket->type == TUNIX_SOCK_STREAM) return "tcp";
    if (socket->type == TUNIX_SOCK_DGRAM) return "udp";
    return "raw";
}

static void report_connect(struct inet_socket *socket) {
    if (!socket || socket->event_open) return;
    socket->event_open = 1;
    eventfs_emit_network_connect(socket->event_uid, socket->event_pid,
        event_protocol(socket), socket->local_address, socket->local_port,
        socket->peer_address, socket->peer_port);
}

static void report_close(struct inet_socket *socket) {
    if (!socket || !socket->event_open || socket->event_close) return;
    socket->event_close = 1;
    eventfs_emit_network_close(socket->event_uid, socket->event_pid,
        event_protocol(socket), socket->local_address, socket->local_port,
        socket->peer_address, socket->peer_port);
}

static int port_in_use(uint16_t port) {
    for (struct inet_socket *other = port_bucket(port); other; other = other->port_next)
        if (other->local_port == port) return 1;
    return 0;
}

static uint16_t allocate_port(void) {
    for (unsigned attempt = 0; attempt < 16384; attempt++) {
        uint16_t candidate = next_ephemeral++;
        if (next_ephemeral < 49152) next_ephemeral = 49152;
        if (!port_in_use(candidate)) return candidate;
    }
    return 0;
}

static int bind_ephemeral(struct inet_socket *socket) {
    uint16_t port = allocate_port();
    if (!port || set_local_port(socket, port) != 0) return -1;
    return 0;
}

static uint32_t tcp_iss_salt;

static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static int seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

static uint32_t tcp_generate_iss(void) {
    tcp_iss_salt += 0x9E3779B9U;
    return (uint32_t)(time_uptime_ns() >> 6) ^ tcp_iss_salt;
}

static size_t buffer_limit(size_t wanted) {
    if (wanted < SOCKET_BUFFER_MIN) return SOCKET_BUFFER_MIN;
    return wanted > SOCKET_BUFFER_MAX ? SOCKET_BUFFER_MAX : wanted;
}

static uint8_t window_scale_for(size_t buffer) {
    uint8_t shift = 0;
    while (shift < TCP_MAX_WINDOW_SCALE && ((size_t)0xFFFFU << shift) < buffer) shift++;
    return shift;
}

static uint32_t own_mss(uint32_t peer) {
    size_t mtu = net_path_mtu(peer);
    if (mtu > NET_IPV4_MAX) mtu = NET_IPV4_MAX;
    return (uint32_t)(mtu - NET_IPV4_HEADER - NET_TCP_HEADER);
}

static void tcp_adopt_options(struct inet_socket *s, const struct net_tcp_options *options) {
    struct tcp_control_block *tcp = s->tcp;
    uint32_t limit = own_mss(s->peer_address);
    uint32_t mss = options && options->has_mss && options->mss ? options->mss : TCP_DEFAULT_MSS;
    tcp->send_mss = mss < limit ? mss : limit;
    if (options && options->has_window_scale) {
        tcp->scaling = 1;
        tcp->snd_wscale = options->window_scale;
    } else {
        tcp->scaling = 0;
        tcp->snd_wscale = 0;
        tcp->rcv_wscale = 0;
    }
}

static size_t tcp_rx_space(const struct inet_socket *s) {
    return s->receive_buffer > s->tcp->rx_len ? s->receive_buffer - s->tcp->rx_len : 0;
}

static uint16_t tcp_window_field(struct inet_socket *s, int syn) {
    struct tcp_control_block *tcp = s->tcp;
    size_t space = tcp_rx_space(s);
    uint8_t shift = (!syn && tcp->scaling) ? tcp->rcv_wscale : 0;
    size_t field = space >> shift;
    if (field > 0xFFFFU) field = 0xFFFFU;
    tcp->rcv_wnd_adv = (uint32_t)(field << shift);
    return (uint16_t)field;
}

static void tcp_arm_rto(struct inet_socket *s) {
    struct tcp_control_block *tcp = s->tcp;
    if (!tcp->rto_ns) tcp->rto_ns = TCP_RTO_INIT_NS;
    tcp->rto_deadline_ns = time_uptime_ns() + tcp->rto_ns;
    timer_watch(s);
}

static void tcp_transmit(struct inet_socket *s, uint32_t seq, uint8_t flags,
                         const uint8_t *data, size_t length) {
    struct tcp_control_block *tcp = s->tcp;
    int syn = (flags & TCP_SYN) != 0;
    struct net_tcp_options options;
    memset(&options, 0, sizeof(options));
    if (syn) {
        options.has_mss = 1;
        options.mss = (uint16_t)own_mss(s->peer_address);
        if (!(flags & TCP_ACK) || tcp->scaling) {
            options.has_window_scale = 1;
            options.window_scale = tcp->rcv_wscale;
        }
    }
    uint16_t window = tcp_window_field(s, syn);
    net_send_tcp(s->local_address, s->local_port, s->peer_address, s->peer_port,
                 seq, tcp->rcv_nxt, flags, window, syn ? &options : NULL, data, length);
}

static void tcp_send_ack(struct inet_socket *s) {
    tcp_transmit(s, s->tcp->snd_nxt, TCP_ACK, NULL, 0);
}

static void tcp_send_window_update(struct inet_socket *s) {
    struct tcp_control_block *tcp = s->tcp;
    if (tcp->state != TCP_ESTABLISHED && tcp->state != TCP_FIN_WAIT_1 &&
        tcp->state != TCP_FIN_WAIT_2) return;
    size_t window = tcp_rx_space(s);
    if (window <= tcp->rcv_wnd_adv) return;
    size_t opened = window - tcp->rcv_wnd_adv;
    size_t threshold = 2U * (size_t)tcp->send_mss;
    if (threshold > s->receive_buffer / 2U) threshold = s->receive_buffer / 2U;
    if (opened >= threshold || (tcp->rcv_wnd_adv < tcp->send_mss && window >= tcp->send_mss))
        tcp_send_ack(s);
}

static int sending_state(int state) {
    return state == TCP_ESTABLISHED || state == TCP_CLOSE_WAIT ||
           state == TCP_FIN_WAIT_1 || state == TCP_CLOSING || state == TCP_LAST_ACK;
}

static void tcp_output(struct inet_socket *s) {
    struct tcp_control_block *tcp = s->tcp;
    if (!sending_state(tcp->state)) return;
    while (tcp->tx_sent < tcp->tx_len) {
        uint32_t window = tcp->snd_wnd ? tcp->snd_wnd : 1U;
        if (tcp->tx_sent >= window) break;
        size_t room = (size_t)window - tcp->tx_sent;
        size_t chunk = tcp->tx_len - tcp->tx_sent;
        if (chunk > tcp->send_mss) chunk = tcp->send_mss;
        if (chunk > room) chunk = room;
        if (!chunk) break;
        uint32_t seq = tcp->snd_una + (uint32_t)tcp->tx_sent;
        tcp_transmit(s, seq, TCP_ACK | TCP_PSH, tcp->tx + tcp->tx_sent, chunk);
        tcp->tx_sent += chunk;
        uint32_t end = tcp->snd_una + (uint32_t)tcp->tx_sent;
        if (seq_gt(end, tcp->snd_nxt)) tcp->snd_nxt = end;
        tcp_arm_rto(s);
    }
    if (tcp->fin_queued && !tcp->fin_sent && tcp->tx_sent == tcp->tx_len) {
        uint32_t seq = tcp->snd_una + (uint32_t)tcp->tx_len;
        tcp_transmit(s, seq, TCP_ACK | TCP_FIN, NULL, 0);
        tcp->fin_sent = 1;
        tcp->snd_nxt = seq + 1U;
        tcp_arm_rto(s);
    }
}

static void pending_append(struct inet_socket *listener, struct inet_socket *child) {
    child->listener = listener;
    child->sibling = NULL;
    if (listener->pending_last) listener->pending_last->sibling = child;
    else listener->pending = child;
    listener->pending_last = child;
    listener->pending_total++;
}

static void pending_detach(struct inet_socket *child) {
    struct inet_socket *listener = child->listener;
    if (!listener) return;
    struct inet_socket *previous = NULL;
    struct inet_socket *at = listener->pending;
    while (at && at != child) {
        previous = at;
        at = at->sibling;
    }
    if (at) {
        if (previous) previous->sibling = child->sibling;
        else listener->pending = child->sibling;
        if (listener->pending_last == child) listener->pending_last = previous;
        listener->pending_total--;
    }
    child->listener = NULL;
    child->sibling = NULL;
}

static void tcp_reset_peer(struct inet_socket *s) {
    if (!s->tcp) return;
    net_send_tcp(s->local_address, s->local_port, s->peer_address, s->peer_port,
                 s->tcp->snd_nxt, s->tcp->rcv_nxt, TCP_RST | TCP_ACK, 0, NULL, NULL, 0);
}

static void tcp_free(struct inet_socket *socket) {
    pending_detach(socket);

    struct inet_socket *child = socket->pending;
    socket->pending = socket->pending_last = NULL;
    socket->pending_total = 0;
    while (child) {
        struct inet_socket *next = child->sibling;
        child->listener = NULL;
        child->sibling = NULL;
        if (child->tcp && child->tcp->state != TCP_CLOSED) tcp_reset_peer(child);
        destroy_socket(child);
        child = next;
    }
    destroy_socket(socket);
}

static void tcp_begin_close(struct inet_socket *s) {
    struct tcp_control_block *tcp = s->tcp;
    if (!tcp || tcp->fin_queued) return;
    if (tcp->state == TCP_ESTABLISHED || tcp->state == TCP_CLOSE_WAIT) {
        tcp->fin_queued = 1;
        tcp->state = (tcp->state == TCP_ESTABLISHED) ? TCP_FIN_WAIT_1 : TCP_LAST_ACK;
        tcp_output(s);
    } else if (tcp->state == TCP_SYN_SENT) {
        tcp->state = TCP_CLOSED;
        tcp->rto_deadline_ns = 0;
    }
}

static struct tcp_control_block *tcp_create(struct inet_socket *socket) {
    struct tcp_control_block *tcp = (struct tcp_control_block *)kmalloc(sizeof(*tcp));
    if (!tcp) return NULL;
    memset(tcp, 0, sizeof(*tcp));
    tcp->send_mss = TCP_DEFAULT_MSS;
    tcp->rcv_wscale = window_scale_for(socket->receive_buffer);
    socket->tcp = tcp;
    return tcp;
}

static int tcp_connect(struct inet_socket *socket, uint32_t address, uint16_t port) {
    struct tcp_control_block *tcp = socket->tcp;
    if (tcp) {
        net_poll();
        if (tcp->pending_error) { int e = tcp->pending_error; tcp->pending_error = 0; return e; }
        if (tcp->state == TCP_ESTABLISHED || tcp->state >= TCP_FIN_WAIT_1) {
            socket->connected = 1;
            if (!socket->connect_reported) { socket->connect_reported = 1; return 0; }
            return -EISCONN;
        }
        return -EINPROGRESS;
    }
    if (!address || !port) return -EINVAL;
    const struct net_config *config = net_get_config();

    if (!net_is_loopback(address) && (!config->link_up || !config->interface_up))
        return -ENETDOWN;
    if (!socket->local_port && bind_ephemeral(socket) != 0) return -EADDRINUSE;
    tcp = tcp_create(socket);
    if (!tcp) return -ENOMEM;
    if (!socket->local_address) socket->local_address = net_source_for(address);
    socket->peer_address = address;
    socket->peer_port = port;
    if (connections_enter(socket) != 0) {
        kfree(tcp);
        socket->tcp = NULL;
        return -ENOMEM;
    }
    tcp->iss = tcp_generate_iss();
    tcp->snd_una = tcp->iss;
    tcp->snd_nxt = tcp->iss + 1U;
    tcp->state = TCP_SYN_SENT;
    tcp_transmit(socket, tcp->iss, TCP_SYN, NULL, 0);
    tcp_arm_rto(socket);
    return -EINPROGRESS;
}

static void tcp_enter_time_wait(struct inet_socket *s) {
    struct tcp_control_block *tcp = s->tcp;
    tcp->state = TCP_TIME_WAIT;
    tcp->rto_deadline_ns = 0;
    tcp->time_wait_deadline_ns = time_uptime_ns() + TCP_TIME_WAIT_NS;
    timer_watch(s);
}

static void tcp_go_back(struct inet_socket *s) {
    struct tcp_control_block *tcp = s->tcp;
    tcp->tx_sent = 0;
    tcp->dup_acks = 0;
    tcp_output(s);
}

static void tcp_process_ack(struct inet_socket *s, uint32_t ack, int duplicate) {
    struct tcp_control_block *tcp = s->tcp;
    if (ack == tcp->snd_una) {
        if (duplicate && tcp->tx_sent && ++tcp->dup_acks == TCP_DUP_ACK_LIMIT) tcp_go_back(s);
        return;
    }
    if (!seq_gt(ack, tcp->snd_una) || !seq_le(ack, tcp->snd_nxt)) return;
    uint32_t acked = ack - tcp->snd_una;
    uint32_t data_acked = acked;
    if (tcp->fin_sent && ack == tcp->snd_nxt) {
        tcp->fin_acked = 1;
        if (data_acked > 0) data_acked -= 1U;
    }
    if (data_acked > tcp->tx_len) data_acked = (uint32_t)tcp->tx_len;
    if (data_acked > 0) {
        memmove(tcp->tx, tcp->tx + data_acked, tcp->tx_len - data_acked);
        tcp->tx_len -= data_acked;
        tcp->tx_sent = tcp->tx_sent > data_acked ? tcp->tx_sent - data_acked : 0;
    }
    tcp->snd_una = ack;
    tcp->dup_acks = 0;
    tcp->retransmit_count = 0;
    tcp->rto_ns = TCP_RTO_INIT_NS;
    if (tcp->snd_una == tcp->snd_nxt) tcp->rto_deadline_ns = 0;
    else tcp_arm_rto(s);
}

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

static int buffer_reserve(uint8_t **buffer, size_t *capacity, size_t *head, size_t used,
                          size_t wanted, size_t limit) {
    if (wanted <= *capacity) return 0;
    size_t grown = *capacity ? *capacity * 2U : TCP_BUFFER_INITIAL;
    while (grown < wanted) grown *= 2U;
    if (grown > limit) grown = limit;
    if (grown < wanted) return -1;
    uint8_t *fresh = (uint8_t *)kmalloc(grown);
    if (!fresh) return -1;
    if (used) {
        size_t first = used < *capacity - *head ? used : *capacity - *head;
        memcpy(fresh, *buffer + *head, first);
        memcpy(fresh + first, *buffer, used - first);
    }
    kfree(*buffer);
    *buffer = fresh;
    *capacity = grown;
    *head = 0;
    return 0;
}

static size_t tcp_store(struct inet_socket *s, const uint8_t *payload, size_t length) {
    struct tcp_control_block *tcp = s->tcp;
    size_t take = tcp_rx_space(s);
    if (take > length) take = length;
    if (!take) return 0;
    if (buffer_reserve(&tcp->rx, &tcp->rx_capacity, &tcp->rx_head, tcp->rx_len,
                       tcp->rx_len + take, s->receive_buffer) != 0) {
        take = tcp->rx_capacity - tcp->rx_len;
        if (!take) return 0;
    }
    size_t tail = (tcp->rx_head + tcp->rx_len) % tcp->rx_capacity;
    size_t first = tcp->rx_capacity - tail;
    if (first > take) first = take;
    memcpy(tcp->rx + tail, payload, first);
    memcpy(tcp->rx, payload + first, take - first);
    tcp->rx_len += take;
    return take;
}

static void tcp_input(struct inet_socket *s, uint32_t seq, uint32_t ack, uint8_t flags,
                      uint16_t window, const struct net_tcp_options *options,
                      const uint8_t *payload, size_t length) {
    struct tcp_control_block *tcp = s->tcp;
    if (!tcp) return;

    if (flags & TCP_RST) {
        tcp->pending_error = (tcp->state == TCP_SYN_SENT) ? -ECONNREFUSED : -ECONNRESET;
        tcp->state = TCP_CLOSED;
        tcp->rto_deadline_ns = 0;
        tcp->peer_fin = 1;
        timer_watch(s);
        return;
    }

    uint32_t previous_window = tcp->snd_wnd;
    if (flags & TCP_SYN) tcp->snd_wnd = window;
    else tcp->snd_wnd = (uint32_t)window << (tcp->scaling ? tcp->snd_wscale : 0);

    if (tcp->state == TCP_SYN_SENT) {
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            if (ack != tcp->iss + 1U) return;
            tcp_adopt_options(s, options);
            tcp->irs = seq;
            tcp->rcv_nxt = seq + 1U;
            tcp->snd_una = ack;
            tcp->snd_nxt = ack;
            tcp->state = TCP_ESTABLISHED;
            tcp->rto_deadline_ns = 0;
            s->connected = 1;
            report_connect(s);
            tcp_send_ack(s);
            tcp_output(s);
        }
        return;
    }

    if (tcp->state == TCP_SYN_RECEIVED) {
        if (flags & TCP_SYN) {
            tcp_transmit(s, tcp->iss, TCP_SYN | TCP_ACK, NULL, 0);
            return;
        }
        if (!(flags & TCP_ACK) || ack != tcp->iss + 1U) return;
        tcp->snd_una = ack;
        tcp->snd_nxt = ack;
        tcp->state = TCP_ESTABLISHED;
        tcp->rto_deadline_ns = 0;
        s->connected = 1;
        s->connect_reported = 1;
    }

    if (flags & TCP_ACK)
        tcp_process_ack(s, ack, !length && !(flags & (TCP_SYN | TCP_FIN)) &&
                                tcp->snd_wnd == previous_window);

    if (length > 0 && seq_lt(seq, tcp->rcv_nxt) && seq_gt(seq + (uint32_t)length, tcp->rcv_nxt)) {
        uint32_t skip = tcp->rcv_nxt - seq;
        payload += skip;
        length -= skip;
        seq = tcp->rcv_nxt;
    }
    if (length > 0 && seq == tcp->rcv_nxt && !tcp->peer_fin) {
        size_t take = tcp_store(s, payload, length);
        tcp->rcv_nxt += (uint32_t)take;
        tcp_send_ack(s);
        if (take < length) {
            length = take;
            flags &= (uint8_t)~TCP_FIN;
        }
    } else if (length > 0) {
        tcp_send_ack(s);
    }

    if ((flags & TCP_FIN) && seq + (uint32_t)length == tcp->rcv_nxt && !tcp->peer_fin) {
        tcp->rcv_nxt += 1U;
        tcp->peer_fin = 1;
        tcp_send_ack(s);
    }

    tcp_output(s);
    tcp_advance_close(s);
    if (tcp->state == TCP_CLOSED || tcp->state == TCP_TIME_WAIT) timer_watch(s);
}

static int64_t tcp_send(struct inet_socket *s, const void *data, size_t length) {
    struct tcp_control_block *tcp = s->tcp;
    if (!tcp) return -ENOTCONN;
    if (tcp->pending_error) { int e = tcp->pending_error; tcp->pending_error = 0; return e; }
    if (s->write_shutdown || tcp->fin_queued) return -EPIPE;
    if (tcp->state == TCP_SYN_SENT) return -EAGAIN;
    if (tcp->state != TCP_ESTABLISHED && tcp->state != TCP_CLOSE_WAIT) return -ENOTCONN;
    if (!length) return 0;
    size_t space = s->send_buffer > tcp->tx_len ? s->send_buffer - tcp->tx_len : 0;
    if (!space) { net_poll(); return -EAGAIN; }
    size_t take = length < space ? length : space;
    size_t head = 0;
    if (buffer_reserve(&tcp->tx, &tcp->tx_capacity, &head, tcp->tx_len,
                       tcp->tx_len + take, s->send_buffer) != 0) {
        take = tcp->tx_capacity - tcp->tx_len;
        if (!take) return -ENOMEM;
    }
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
    size_t first = tcp->rx_capacity - tcp->rx_head;
    if (first > take) first = take;
    memcpy(out, tcp->rx + tcp->rx_head, first);
    memcpy(out + first, tcp->rx, take - first);
    if (!(flags & MSG_PEEK)) {
        tcp->rx_head = (tcp->rx_head + take) % tcp->rx_capacity;
        tcp->rx_len -= take;
        if (!tcp->rx_len) tcp->rx_head = 0;
        tcp_send_window_update(s);
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
    if (tcp->snd_una == tcp->snd_nxt) {
        tcp->rto_deadline_ns = 0;
        tcp->retransmit_count = 0;
        return;
    }
    tcp->rto_ns = tcp->rto_ns ? tcp->rto_ns * 2U : TCP_RTO_INIT_NS;
    if (tcp->rto_ns > TCP_RTO_MAX_NS) tcp->rto_ns = TCP_RTO_MAX_NS;
    tcp->rto_deadline_ns = time_uptime_ns() + tcp->rto_ns;
    switch (tcp->state) {
        case TCP_SYN_SENT:
            tcp_transmit(s, tcp->iss, TCP_SYN, NULL, 0);
            break;
        case TCP_SYN_RECEIVED:
            tcp_transmit(s, tcp->iss, TCP_SYN | TCP_ACK, NULL, 0);
            break;
        default:
            if (!sending_state(tcp->state)) break;
            if (tcp->tx_len)
                tcp_go_back(s);
            else if (tcp->fin_sent && !tcp->fin_acked)
                tcp_transmit(s, tcp->snd_nxt - 1U, TCP_ACK | TCP_FIN, NULL, 0);
            break;
    }
}

static int timer_pending(const struct inet_socket *s) {
    const struct tcp_control_block *tcp = s->tcp;
    return tcp->rto_deadline_ns || tcp->state == TCP_TIME_WAIT || s->orphan ||
           (s->listener && tcp->state == TCP_CLOSED);
}

void inet_socket_tcp_timer_poll(void) {
    NET_LOCKED;
    uint64_t now = time_uptime_ns();
    struct inet_socket *s = timed_sockets;
    while (s) {
        timer_cursor = s->timer_next;
        struct tcp_control_block *tcp = s->tcp;
        if (tcp->state == TCP_TIME_WAIT && now >= tcp->time_wait_deadline_ns) {
            tcp->state = TCP_CLOSED;
            tcp->rto_deadline_ns = 0;
        }
        if (tcp->rto_deadline_ns && now >= tcp->rto_deadline_ns) tcp_retransmit(s);
        if ((s->orphan && (tcp->state == TCP_CLOSED || now >= tcp->orphan_deadline_ns)) ||
            (s->listener && tcp->state == TCP_CLOSED)) {
            tcp_free(s);
        } else if (!timer_pending(s)) {
            timer_unwatch(s);
        }
        s = timer_cursor;
    }
    timer_cursor = NULL;
}

static struct inet_socket *tcp_open_child(struct inet_socket *listener, uint32_t source,
                                          uint16_t source_port, uint32_t destination,
                                          uint16_t destination_port, uint32_t seq,
                                          uint16_t window,
                                          const struct net_tcp_options *options) {
    if (listener->pending_total >= listener->backlog) return NULL;
    struct inet_socket *child = (struct inet_socket *)kmalloc(sizeof(*child));
    if (!child) return NULL;
    memset(child, 0, sizeof(*child));
    child->refs = 1;
    child->domain = TUNIX_AF_INET;
    child->type = TUNIX_SOCK_STREAM;
    child->protocol = listener->protocol;
    child->ttl = 64;
    child->receive_buffer = listener->receive_buffer;
    child->send_buffer = listener->send_buffer;
    child->local_address = destination;
    child->local_port = destination_port;
    child->peer_address = source;
    child->peer_port = source_port;
    if (!tcp_create(child)) {
        kfree(child);
        return NULL;
    }
    register_socket(child);
    if (connections_enter(child) != 0) {
        destroy_socket(child);
        return NULL;
    }

    struct tcp_control_block *tcp = child->tcp;
    tcp_adopt_options(child, options);
    tcp->snd_wnd = window;
    tcp->irs = seq;
    tcp->rcv_nxt = seq + 1U;
    tcp->iss = tcp_generate_iss();
    tcp->snd_una = tcp->iss;
    tcp->snd_nxt = tcp->iss + 1U;
    tcp->state = TCP_SYN_RECEIVED;
    pending_append(listener, child);
    tcp_transmit(child, tcp->iss, TCP_SYN | TCP_ACK, NULL, 0);
    tcp_arm_rto(child);
    return child;
}

void inet_socket_receive_tcp(uint32_t source, uint16_t source_port, uint32_t destination,
                             uint16_t destination_port, uint32_t seq, uint32_t ack, uint8_t flags,
                             uint16_t window, const struct net_tcp_options *options,
                             const uint8_t *payload, size_t length) {
    NET_LOCKED;
    if (connections.buckets) {
        uint32_t key = connection_key_of(destination_port, source, source_port);
        for (struct inet_socket *s = connections.buckets[key & connections.mask]; s;
             s = s->connection_next) {
            if (!s->tcp || s->local_port != destination_port) continue;
            if (s->local_address && s->local_address != destination) continue;
            if (s->peer_port != source_port || s->peer_address != source) continue;
            tcp_input(s, seq, ack, flags, window, options, payload, length);
            return;
        }
    }

    if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN) {
        for (struct inet_socket *s = port_bucket(destination_port); s; s = s->port_next) {
            if (!s->listening || s->type != TUNIX_SOCK_STREAM) continue;
            if (s->local_port != destination_port) continue;
            if (s->local_address && s->local_address != destination) continue;

            (void)tcp_open_child(s, source, source_port, destination,
                                 destination_port, seq, window, options);
            return;
        }
    }

    if (!(flags & TCP_RST)) {
        uint32_t rst_seq = (flags & TCP_ACK) ? ack : 0U;
        uint32_t rst_ack = seq + (uint32_t)length + ((flags & (TCP_SYN | TCP_FIN)) ? 1U : 0U);
        uint8_t rst_flags = (flags & TCP_ACK) ? TCP_RST : (TCP_RST | TCP_ACK);
        net_send_tcp(destination, destination_port, source, source_port,
                     rst_seq, rst_ack, rst_flags, 0, NULL, NULL, 0);
    }
}

struct inet_socket *inet_socket_create(int domain, int type, int protocol) {
    NET_LOCKED;
    int base_type = type & 0xFU;
    if (domain == TUNIX_AF_INET) {
        if (base_type == TUNIX_SOCK_STREAM) {
            if (protocol != 0 && protocol != 6) return NULL;
        } else if (base_type == TUNIX_SOCK_DGRAM) {
            if (protocol != 0 && protocol != 17 && protocol != IPPROTO_ICMP) return NULL;
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
    size_t buffer = base_type == TUNIX_SOCK_STREAM ? STREAM_BUFFER_DEFAULT
                                                   : DATAGRAM_BUFFER_DEFAULT;
    socket->receive_buffer = buffer;
    socket->send_buffer = buffer;
    register_socket(socket);
    return socket;
}

void inet_socket_ref(struct inet_socket *socket) { if (socket) socket->refs++; }
void inet_socket_unref(struct inet_socket *socket) {
    NET_LOCKED;
    if (!socket || --socket->refs > 0) return;
    report_close(socket);

    if (socket->tcp && !socket->orphan && socket->tcp->state != TCP_CLOSED) {
        tcp_begin_close(socket);
        if (socket->tcp->state != TCP_CLOSED) {
            socket->orphan = 1;
            socket->tcp->orphan_deadline_ns = time_uptime_ns() + TCP_ORPHAN_NS;
            timer_watch(socket);
            return;
        }
    }
    tcp_free(socket);
}

static int local_port_conflict(struct inet_socket *socket, uint32_t address, uint16_t port) {
    for (struct inet_socket *other = port_bucket(port); other; other = other->port_next) {
        if (other == socket || other->domain != TUNIX_AF_INET ||
            other->type != socket->type || other->local_port != port) continue;
        if (is_ping_socket(other) != is_ping_socket(socket)) continue;
        if (!other->local_address || !address || other->local_address == address) return 1;
    }
    return 0;
}

int inet_socket_bind(struct inet_socket *socket, const void *address, size_t length) {
    NET_LOCKED;
    if (!socket || !address) return -EINVAL;
    if (socket->domain == TUNIX_AF_INET) {
        if (length < sizeof(struct tunix_sockaddr_in)) return -EINVAL;
        const struct tunix_sockaddr_in *in = (const struct tunix_sockaddr_in *)address;
        if (in->family != TUNIX_AF_INET) return -EAFNOSUPPORT;
        uint16_t port = net_htons(in->port);
        if (port && local_port_conflict(socket, in->address, port)) return -EADDRINUSE;
        socket->local_address = in->address;
        if (!port && is_ping_socket(socket)) {
            if (bind_ephemeral(socket) != 0) return -EADDRINUSE;
        } else if (set_local_port(socket, port) != 0) {
            return -ENOMEM;
        }
        socket->bound = 1;
        return 0;
    }
    if (socket->domain == TUNIX_AF_PACKET) {
        if (length < sizeof(struct tunix_sockaddr_ll)) return -EINVAL;
        const struct tunix_sockaddr_ll *ll = (const struct tunix_sockaddr_ll *)address;
        if (ll->family != TUNIX_AF_PACKET ||
            (ll->ifindex != 0 && ll->ifindex != NET_IFINDEX_ETH0))
            return -EADDRNOTAVAIL;
        if (ll->protocol) socket->protocol = ll->protocol;
        socket->bound = 1;
        return 0;
    }
    return -EAFNOSUPPORT;
}

int inet_socket_is_stream(const struct inet_socket *socket) {
    NET_LOCKED;
    return socket && socket->type == TUNIX_SOCK_STREAM;
}

int inet_socket_is_listener(struct inet_socket *socket) {
    NET_LOCKED;
    return socket && socket->listening;
}

int inet_socket_listen(struct inet_socket *socket, int backlog) {
    NET_LOCKED;
    if (!socket || socket->domain != TUNIX_AF_INET ||
        socket->type != TUNIX_SOCK_STREAM) return -EOPNOTSUPP;
    if (socket->tcp) return -EINVAL;
    if (!socket->local_port && bind_ephemeral(socket) != 0) return -EADDRINUSE;
    unsigned wanted = backlog <= 0 ? 1U : (unsigned)backlog;
    socket->backlog = wanted > TCP_BACKLOG_MAX ? TCP_BACKLOG_MAX : wanted;
    socket->listening = 1;
    return 0;
}

struct inet_socket *inet_socket_accept(struct inet_socket *listener) {
    NET_LOCKED;
    if (!listener || !listener->listening) return NULL;
    net_poll();
    for (struct inet_socket *s = listener->pending; s; s = s->sibling) {
        if (!s->tcp || s->tcp->state == TCP_SYN_RECEIVED) continue;

        if (s->tcp->state == TCP_CLOSED) continue;
        pending_detach(s);
        s->connected = 1;
        s->connect_reported = 1;
        return s;
    }
    return NULL;
}

int inet_socket_connect(struct inet_socket *socket, const void *address, size_t length,
                        uint64_t pid, uint32_t uid) {
    NET_LOCKED;
    if (!socket || socket->domain != TUNIX_AF_INET || !address ||
        length < sizeof(struct tunix_sockaddr_in)) return -EINVAL;
    const struct tunix_sockaddr_in *in = (const struct tunix_sockaddr_in *)address;
    if (in->family != TUNIX_AF_INET) return -EAFNOSUPPORT;
    socket->event_pid = pid;
    socket->event_uid = uid;
    if (socket->type == TUNIX_SOCK_STREAM)
        return tcp_connect(socket, in->address, net_htons(in->port));
    if (!socket->local_port && bind_ephemeral(socket) != 0) return -EADDRINUSE;
    socket->peer_address = in->address;
    socket->peer_port = net_htons(in->port);
    socket->connected = 1;
    report_connect(socket);
    return 0;
}

void inet_socket_report_accept(struct inet_socket *socket, uint64_t pid,
                               uint32_t uid) {
    NET_LOCKED;
    if (!socket || socket->event_open) return;
    socket->event_pid = pid;
    socket->event_uid = uid;
    socket->event_open = 1;
    eventfs_emit_network_accept(uid, pid, event_protocol(socket),
        socket->local_address, socket->local_port, socket->peer_address,
        socket->peer_port);
}

static int enqueue_with_ttl(struct inet_socket *socket, const void *data, size_t length,
                            const void *address, size_t address_length, uint8_t ttl) {
    if (!socket || socket->read_shutdown) return -EAGAIN;
    if (socket->queue_bytes + length > socket->receive_buffer) return -EAGAIN;
    struct datagram *item = (struct datagram *)kmalloc(sizeof(*item) + length);
    if (!item) return -ENOMEM;
    item->next = NULL;
    item->ttl = ttl;
    item->length = length;
    memcpy(item->data, data, length);
    item->address_length = address_length > sizeof(item->address) ? sizeof(item->address) : address_length;
    if (address && item->address_length) memcpy(item->address, address, item->address_length);
    if (socket->queue_last) socket->queue_last->next = item;
    else socket->queue_first = item;
    socket->queue_last = item;
    socket->queue_bytes += length;
    return 0;
}

static int enqueue(struct inet_socket *socket, const void *data, size_t length,
                   const void *address, size_t address_length) {
    return enqueue_with_ttl(socket, data, length, address, address_length, 0);
}

int64_t inet_socket_sendto(struct inet_socket *socket, const void *data, size_t length, int flags,
                           const void *address, size_t address_length) {
    NET_LOCKED;
    (void)flags;
    if (!socket || !data) return -EINVAL;
    if (socket->type == TUNIX_SOCK_STREAM) return tcp_send(socket, data, length);
    if (socket->write_shutdown) return -EPIPE;
    const struct net_config *config = net_get_config();
    int link = config->link_up && config->interface_up;
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
        if (!link && !net_is_loopback(destination)) return -ENETDOWN;
        if (length > NET_IPV4_MAX - NET_IPV4_HEADER - 8U) return -EMSGSIZE;
        if (!socket->local_port && bind_ephemeral(socket) != 0) return -EADDRINUSE;
        if (is_ping_socket(socket)) {
            if (length < sizeof(struct icmp_message)) return -EINVAL;
            const struct icmp_message *wanted = (const struct icmp_message *)data;
            if (wanted->type != ICMP_ECHO) return -EINVAL;
            uint8_t *message = (uint8_t *)kmalloc(length);
            if (!message) return -ENOMEM;
            memcpy(message, data, length);
            struct icmp_message *head = (struct icmp_message *)message;
            head->id = net_htons(socket->local_port);
            head->checksum = 0;
            head->checksum = net_htons(net_checksum(message, length));
            int status = net_send_ipv4(destination, IPPROTO_ICMP, message, length,
                                       socket->ttl, 0);
            kfree(message);
            if (status != 0) return -EAGAIN;
        } else if (socket->type == TUNIX_SOCK_DGRAM) {
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
        if (!link) return -ENETDOWN;
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
    NET_LOCKED;
    if (!socket || !data) return -EINVAL;
    if (socket->type == TUNIX_SOCK_STREAM) return tcp_recv(socket, data, length, flags);
    if (socket->read_shutdown) return 0;
    net_poll();
    struct datagram *item = socket->queue_first;
    if (!item) return -EAGAIN;
    size_t amount = length < item->length ? length : item->length;
    memcpy(data, item->data, amount);
    if (address && address_length) {
        size_t copy = *address_length < item->address_length ? *address_length : item->address_length;
        memcpy(address, item->address, copy);
        *address_length = item->address_length;
    }
    socket->last_ttl = item->ttl;
    if (!(flags & MSG_PEEK)) {
        socket->queue_first = item->next;
        if (!socket->queue_first) socket->queue_last = NULL;
        socket->queue_bytes -= item->length;
        kfree(item);
    }
    return (int64_t)amount;
}

int inet_socket_wants_ttl(struct inet_socket *socket) {
    NET_LOCKED;
    return socket && socket->recv_ttl;
}

uint8_t inet_socket_last_ttl(struct inet_socket *socket) {
    NET_LOCKED;
    return socket ? socket->last_ttl : 0;
}

int inet_socket_getsockname(struct inet_socket *socket, void *address, size_t *length) {
    NET_LOCKED;
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
    NET_LOCKED;
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

static void report_refused_option(const char *what, int level, int option) {
    static struct { int level; int option; } seen[16];
    static unsigned count;
    for (unsigned i = 0; i < count; i++)
        if (seen[i].level == level && seen[i].option == option) return;
    if (count < 16U) {
        seen[count].level = level;
        seen[count].option = option;
        count++;
    }
    kprintf("INET: %s level %d option %d refused\n", what, level, option);
}

int inet_socket_setsockopt(struct inet_socket *socket, int level, int option,
                           const void *value, size_t length) {
    NET_LOCKED;
    if (!socket) return -EINVAL;
    if (level == SOL_SOCKET) {
        if (option == SO_BROADCAST && value && length >= sizeof(int)) {
            socket->broadcast = *(const int *)value != 0; return 0;
        }
        if ((option == SO_RCVBUF || option == SO_SNDBUF) && value && length >= sizeof(int)) {
            int wanted = *(const int *)value;
            size_t size = buffer_limit(wanted > 0 ? (size_t)wanted * 2U : 0);
            if (option == SO_RCVBUF) socket->receive_buffer = size;
            else socket->send_buffer = size;
            if (socket->tcp && option == SO_RCVBUF && socket->tcp->state == TCP_ESTABLISHED)
                tcp_send_window_update(socket);
            return 0;
        }
        if (option == SO_BINDTODEVICE || option == SO_ATTACH_FILTER ||
            option == SO_RCVTIMEO || option == SO_SNDTIMEO) return 0;
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

        if (option == IP_RECVERR && value && length >= sizeof(int)) {
            socket->report_errors = *(const int *)value != 0;
            return 0;
        }
        if (option == IP_RECVTTL && value && length >= sizeof(int)) {
            socket->recv_ttl = *(const int *)value != 0;
            return 0;
        }
        if (option == IP_RETOPTS && value && length >= sizeof(int)) {
            socket->return_options = *(const int *)value != 0;
            return 0;
        }
        if (option == IP_PKTINFO || option == IP_MTU_DISCOVER || option == IP_RECVTOS) return 0;
    }

    if (level == IPPROTO_TCP && (option == TCP_NODELAY || option == TCP_KEEPIDLE ||
                                 option == TCP_KEEPINTVL || option == TCP_KEEPCNT)) return 0;
    if (level == SOL_RAW && option == ICMP_FILTER) {
        if (!value || length < sizeof(uint32_t)) return -EINVAL;
        socket->icmp_filter = *(const uint32_t *)value;
        return 0;
    }
    if (level == SOL_PACKET && option == PACKET_AUXDATA) return 0;
    if (level == SOL_UDP && option == UDP_GRO) return 0;

    report_refused_option("setsockopt", level, option);
    return -EOPNOTSUPP;
}

int inet_socket_getsockopt(struct inet_socket *socket, int level, int option,
                           void *value, size_t *length) {
    NET_LOCKED;
    if (!socket || !value || !length || *length < sizeof(int)) return -EINVAL;
    int result = 0;
    if (level == SOL_SOCKET && option == SO_ERROR) {
        if (socket->tcp && socket->tcp->pending_error) {
            result = -socket->tcp->pending_error;
            socket->tcp->pending_error = 0;
        }
    }
    else if (level == SOL_SOCKET && option == SO_BROADCAST) result = socket->broadcast;
    else if (level == SOL_SOCKET && option == SO_RCVBUF) result = (int)socket->receive_buffer;
    else if (level == SOL_SOCKET && option == SO_SNDBUF) result = (int)socket->send_buffer;
    else if (level == IPPROTO_IP && option == IP_TTL) result = socket->ttl;
    else if (level == IPPROTO_IP && option == IP_RECVERR) result = socket->report_errors;
    else if (level == IPPROTO_IP && option == IP_RECVTTL) result = socket->recv_ttl;
    else if (level == IPPROTO_IP && option == IP_RETOPTS) result = socket->return_options;

    else if (level == IPPROTO_TCP && option == TCP_NODELAY) result = 1;
    else if (level == SOL_RAW && option == ICMP_FILTER) result = (int)socket->icmp_filter;
    else {
        report_refused_option("getsockopt", level, option);
        return -EOPNOTSUPP;
    }
    memcpy(value, &result, sizeof(result));
    *length = sizeof(result);
    return 0;
}

static int ifname_valid(const uint8_t *argument) {
    return (!argument[0]) || (argument[0] == 'e' && argument[1] == 't' &&
        argument[2] == 'h' && argument[3] == '0' && argument[4] == 0) ||
        (argument[0] == 'l' && argument[1] == 'o' && argument[2] == 0);
}

static int ifname_loopback(const uint8_t *argument) {
    return argument[0] == 'l' && argument[1] == 'o' && argument[2] == 0;
}

static void set_sockaddr(uint8_t *where, uint32_t address) {
    memset(where, 0, 16);
    where[0] = TUNIX_AF_INET;
    memcpy(where + 4, &address, 4);
}

int inet_socket_ioctl(struct inet_socket *socket, unsigned long request, void *argument) {
    NET_LOCKED;
    if (!socket) return -EINVAL;
    return net_interface_ioctl(request, argument);
}

int net_interface_ioctl(unsigned long request, void *argument) {
    NET_LOCKED;
    if (!argument) return -EINVAL;
    uint8_t *arg = (uint8_t *)argument;
    const struct net_config *cfg = net_get_config();
    if (request == SIOCADDRT || request == SIOCDELRT) {
        if (request == SIOCADDRT) {
            uint32_t gateway;
            memcpy(&gateway, arg + 28, 4);
            if (gateway) net_set_gateway(gateway);
        }
        return 0;
    }

    if (request == SIOCGIFNAME) {
        int index;
        memcpy(&index, arg + 16, sizeof(index));
        const char *name = index == NET_IFINDEX_LO ? "lo" :
                           index == NET_IFINDEX_ETH0 ? "eth0" : NULL;
        if (!name) return -ENODEV;
        memset(arg, 0, 16);
        memcpy(arg, name, strlen(name) + 1);
        return 0;
    }
    if (!ifname_valid(arg)) return -EADDRNOTAVAIL;
    if (!arg[0]) { arg[0]='e'; arg[1]='t'; arg[2]='h'; arg[3]='0'; arg[4]=0; }
    int loopback = ifname_loopback(arg);
    switch (request) {
        case SIOCGIFFLAGS: {
            int16_t flags = loopback ? IFF_UP | IFF_LOOPBACK | IFF_RUNNING
                                     : IFF_BROADCAST | IFF_MULTICAST;
            if (loopback) { memcpy(arg + 16, &flags, sizeof(flags)); return 0; }
            if (cfg->interface_up) flags |= IFF_UP;
            if (cfg->link_up) flags |= IFF_RUNNING;
            memcpy(arg + 16, &flags, sizeof(flags)); return 0;
        }
        case SIOCSIFFLAGS: {
            if (loopback) return 0;
            int16_t flags; memcpy(&flags, arg + 16, sizeof(flags));
            net_set_interface_up((flags & IFF_UP) != 0); return 0;
        }
        case SIOCGIFADDR: set_sockaddr(arg + 16, loopback ? net_htonl(0x7F000001U) : cfg->address); return 0;
        case SIOCSIFADDR: { if (loopback) return 0; uint32_t value; memcpy(&value, arg + 20, 4); net_set_address(value); return 0; }
        case SIOCGIFNETMASK: set_sockaddr(arg + 16, loopback ? net_htonl(0xFF000000U) : cfg->netmask); return 0;
        case SIOCSIFNETMASK: { if (loopback) return 0; uint32_t value; memcpy(&value, arg + 20, 4); net_set_netmask(value); return 0; }
        case SIOCGIFBRDADDR: set_sockaddr(arg + 16, loopback ? net_htonl(0x7FFFFFFFU) : cfg->address | ~cfg->netmask); return 0;
        case SIOCGIFHWADDR:
            memset(arg + 16, 0, 16);
            if (loopback) { uint16_t type = 772; memcpy(arg + 16, &type, sizeof(type)); }
            else { arg[16] = 1; memcpy(arg + 18, cfg->mac, 6); }
            return 0;
        case SIOCGIFINDEX: { int index = loopback ? NET_IFINDEX_LO : NET_IFINDEX_ETH0; memcpy(arg + 16, &index, 4); return 0; }
        case SIOCGIFMTU: { int mtu = loopback ? 65536 : 1500; memcpy(arg + 16, &mtu, 4); return 0; }

        case SIOCGIFTXQLEN: { int txqlen = 1000; memcpy(arg + 16, &txqlen, 4); return 0; }
        default: return -ENOTTY;
    }
}

int inet_socket_read_ready(struct inet_socket *socket) {
    NET_LOCKED;
    net_poll();
    if (!socket) return 0;

    if (socket->listening) {
        for (struct inet_socket *s = socket->pending; s; s = s->sibling)
            if (s->tcp && s->tcp->state != TCP_SYN_RECEIVED &&
                s->tcp->state != TCP_CLOSED) return 1;
        return 0;
    }
    if (socket->tcp) {
        struct tcp_control_block *tcp = socket->tcp;
        return tcp->rx_len > 0 || tcp->peer_fin || tcp->pending_error ||
               tcp->state == TCP_CLOSED || socket->read_shutdown;
    }
    return socket->read_shutdown || socket->queue_first != NULL;
}
int inet_socket_write_ready(struct inet_socket *socket) {
    NET_LOCKED;
    const struct net_config *cfg = net_get_config();
    if (!socket) return 0;
    if (socket->tcp) {
        struct tcp_control_block *tcp = socket->tcp;
        if (tcp->state == TCP_ESTABLISHED || tcp->state == TCP_CLOSE_WAIT)
            return tcp->tx_len < socket->send_buffer;

        return tcp->state == TCP_CLOSED || tcp->pending_error != 0;
    }
    return !socket->write_shutdown && cfg->link_up && cfg->interface_up;
}
int inet_socket_peer_closed(struct inet_socket *socket) {
    NET_LOCKED;
    return socket && socket->tcp && (socket->tcp->peer_fin || socket->tcp->pending_error);
}
int inet_socket_shutdown(struct inet_socket *socket, int how) {
    NET_LOCKED;
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
        queue_clear(socket);
    }
    if (how == 1 || how == 2) socket->write_shutdown = 1;
    return 0;
}
int64_t inet_socket_read(struct inet_socket *socket, size_t length, void *data) {
    NET_LOCKED;
    return inet_socket_recvfrom(socket, data, length, 0, NULL, NULL);
}
int64_t inet_socket_write(struct inet_socket *socket, size_t length, const void *data) {
    NET_LOCKED;
    return inet_socket_sendto(socket, data, length, 0, NULL, 0);
}

void inet_socket_receive_udp(const uint8_t *payload, size_t length, uint32_t source,
                             uint16_t source_port, uint32_t destination, uint16_t destination_port) {
    NET_LOCKED;
    struct tunix_sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.family = TUNIX_AF_INET;
    address.port = net_htons(source_port);
    address.address = source;
    for (struct inet_socket *socket = port_bucket(destination_port); socket;
         socket = socket->port_next) {
        if (socket->domain != TUNIX_AF_INET || socket->type != TUNIX_SOCK_DGRAM) continue;
        if (is_ping_socket(socket)) continue;
        if (socket->local_port != destination_port) continue;
        if (socket->local_address && socket->local_address != destination) continue;
        if (socket->connected && (socket->peer_address != source || socket->peer_port != source_port)) continue;
        (void)enqueue(socket, payload, length, &address, sizeof(address));
    }
}

void inet_socket_receive_ipv4(const uint8_t *packet, size_t length, uint8_t protocol,
                              uint32_t source, uint32_t destination) {
    NET_LOCKED;
    (void)destination;
    struct tunix_sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.family = TUNIX_AF_INET;
    address.address = source;
    unsigned header_length = length ? (unsigned)(packet[0] & 0x0FU) * 4U : 0U;
    uint8_t hops = length > 8U ? packet[8] : 0U;
    const struct icmp_message *icmp = NULL;
    if (protocol == IPPROTO_ICMP && header_length >= 20U &&
        length >= header_length + sizeof(struct icmp_message))
        icmp = (const struct icmp_message *)(packet + header_length);

    for (struct inet_socket *socket = raw_sockets; socket; socket = socket->class_next) {
        if (icmp && (socket->icmp_filter & (1U << icmp->type))) continue;
        if (socket->type == TUNIX_SOCK_RAW) {
            if (socket->protocol && socket->protocol != protocol) continue;
            (void)enqueue_with_ttl(socket, packet, length, &address, sizeof(address), hops);
            continue;
        }
        if (!icmp || icmp->type != ICMP_ECHO_REPLY) continue;
        if (net_htons(icmp->id) != socket->local_port) continue;
        (void)enqueue_with_ttl(socket, packet + header_length, length - header_length,
                               &address, sizeof(address), hops);
    }
}

void inet_socket_receive_ethernet(const uint8_t *frame, size_t length, uint16_t ethertype) {
    NET_LOCKED;
    if (length < 14 || !packet_sockets) return;
    struct tunix_sockaddr_ll address;
    memset(&address, 0, sizeof(address));
    address.family = TUNIX_AF_PACKET;
    address.protocol = net_htons(ethertype);
    address.ifindex = NET_IFINDEX_ETH0;
    address.hatype = 1;
    address.halen = 6;
    memcpy(address.address, frame + 6, 6);
    for (struct inet_socket *socket = packet_sockets; socket; socket = socket->class_next) {
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
    NET_LOCKED;
    text_string(buffer, capacity, length, "  sl  local_address rem_address   st\n");
    unsigned slot = 0;
    for (struct inet_socket *s = all_sockets; s; s = s->all_next) {
        if (s->domain != TUNIX_AF_INET || s->type != TUNIX_SOCK_DGRAM) continue;
        text_char(buffer, capacity, length, ' '); text_hex4(buffer, capacity, length, (uint16_t)slot++);
        text_string(buffer, capacity, length, ": "); text_hex8(buffer, capacity, length, s->local_address);
        text_char(buffer, capacity, length, ':'); text_hex4(buffer, capacity, length, s->local_port);
        text_char(buffer, capacity, length, ' '); text_hex8(buffer, capacity, length, s->peer_address);
        text_char(buffer, capacity, length, ':'); text_hex4(buffer, capacity, length, s->peer_port);
        text_string(buffer, capacity, length, " 07\n");
    }
}
void inet_socket_proc_raw(char *buffer, size_t capacity, size_t *length) {
    NET_LOCKED;
    text_string(buffer, capacity, length, "  sl  local_address rem_address   st\n");
    unsigned slot = 0;
    for (struct inet_socket *s = all_sockets; s; s = s->all_next) {
        if (s->domain != TUNIX_AF_INET || s->type != TUNIX_SOCK_RAW) continue;
        text_char(buffer, capacity, length, ' '); text_hex4(buffer, capacity, length, (uint16_t)slot++);
        text_string(buffer, capacity, length, ": "); text_hex8(buffer, capacity, length, s->local_address);
        text_string(buffer, capacity, length, ":0000 00000000:0000 07\n");
    }
}
void inet_socket_proc_tcp(char *buffer, size_t capacity, size_t *length) {
    NET_LOCKED;
    static const char *const codes[] = {
        "07", "02", "03", "01", "04", "05", "0B", "06", "08", "09"
    };
    text_string(buffer, capacity, length, "  sl  local_address rem_address   st\n");
    unsigned slot = 0;
    for (struct inet_socket *s = all_sockets; s; s = s->all_next) {
        if (s->type != TUNIX_SOCK_STREAM) continue;
        if (!s->tcp && !s->listening) continue;
        text_char(buffer, capacity, length, ' '); text_hex4(buffer, capacity, length, (uint16_t)slot++);
        text_string(buffer, capacity, length, ": "); text_hex8(buffer, capacity, length, s->local_address);
        text_char(buffer, capacity, length, ':'); text_hex4(buffer, capacity, length, s->local_port);
        text_char(buffer, capacity, length, ' '); text_hex8(buffer, capacity, length, s->peer_address);
        text_char(buffer, capacity, length, ':'); text_hex4(buffer, capacity, length, s->peer_port);
        text_char(buffer, capacity, length, ' ');
        int state = s->tcp ? s->tcp->state : -1;

        text_string(buffer, capacity, length,
                    state < 0 ? "0A" : ((state <= 9) ? codes[state] : "07"));
        text_char(buffer, capacity, length, '\n');
    }
}
