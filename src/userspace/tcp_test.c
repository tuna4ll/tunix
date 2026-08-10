/*
 * Can this machine talk to itself over TCP?
 *
 * Two things are under test and each is useless without the other: packets
 * addressed to 127.0.0.1 have to come back up the stack instead of going to the
 * adapter, and a socket has to be able to wait for a connection rather than
 * only make one. So every check here runs between two processes on this
 * machine -- a server that binds, listens and accepts, and a forked client that
 * connects -- which is exactly the shape that could not be built before.
 *
 * The bulk transfer at the end is the one that matters most: 64 KiB is four
 * times the receive ring, so it can only arrive if the window opens and closes
 * correctly as the reader drains it.
 */
#include "tunix_libc.h"

#define PORT 18080
#define DEAD_PORT 18081
#define UDP_PORT 18082
#define BULK_BYTES 65536U

static int failures;

static void say(const char *text) {
    t_puts(text);
    int fd = t_open("/dev/kmsg", T_O_WRONLY, 0);
    if (fd < 0) return;
    (void)t_write(fd, text, t_strlen(text));
    t_close(fd);
}

static void check(int ok, const char *what) {
    say(ok ? "TCPTEST: ok   " : "TCPTEST: FAIL ");
    say(what);
    say("\n");
    if (!ok) failures++;
}

static uint16_t port_be(uint16_t port) {
    return (uint16_t)((port << 8) | (port >> 8));
}

static void loopback_address(struct t_sockaddr_in *out, uint16_t port) {
    out->family = 2;                     /* AF_INET */
    out->port = port_be(port);
    /* 127.0.0.1, already in network order. */
    out->address = 0x0100007FU;
    for (unsigned i = 0; i < sizeof(out->zero); i++) out->zero[i] = 0;
}

/* Read exactly `size` bytes, yielding while the peer is still sending. */
static long read_fully(int fd, uint8_t *buffer, size_t size) {
    size_t got = 0;
    while (got < size) {
        long chunk = t_read(fd, buffer + got, size - got);
        if (chunk == 0) break;
        if (chunk < 0) {
            if (chunk == -11) { t_yield(); continue; }   /* EAGAIN */
            return chunk;
        }
        got += (size_t)chunk;
    }
    return (long)got;
}

static long write_fully(int fd, const uint8_t *buffer, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        long chunk = t_write(fd, buffer + sent, size - sent);
        if (chunk < 0) {
            if (chunk == -11) { t_yield(); continue; }
            return chunk;
        }
        if (chunk == 0) break;
        sent += (size_t)chunk;
    }
    return (long)sent;
}

/* The client half, run in a forked child: connect, exchange, stream. */
static void client(void) {
    struct t_sockaddr_in server;
    loopback_address(&server, PORT);

    int fd = t_socket(2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
    if (fd < 0) t_exit(1);
    if (t_connect_in(fd, &server) != 0) t_exit(2);
    if (write_fully(fd, (const uint8_t *)"ping", 4) != 4) t_exit(3);

    uint8_t reply[4];
    if (read_fully(fd, reply, 4) != 4) t_exit(4);
    if (reply[0] != 'p' || reply[1] != 'o' || reply[2] != 'n' || reply[3] != 'g')
        t_exit(5);

    /* Stream the bulk payload, then let the server see the end of it. */
    static uint8_t bulk[BULK_BYTES];
    for (unsigned i = 0; i < BULK_BYTES; i++) bulk[i] = (uint8_t)(i * 31U + (i >> 8));
    if (write_fully(fd, bulk, BULK_BYTES) != (long)BULK_BYTES) t_exit(6);
    t_shutdown(fd, 1);
    t_close(fd);
    t_exit(0);
}

static void run_connection_tests(void) {
    struct t_sockaddr_in bind_address, peer;
    loopback_address(&bind_address, PORT);

    int listener = t_socket(2, 1, 0);
    check(listener >= 0, "opened a stream socket");
    check(t_bind_in(listener, &bind_address) == 0, "bound to 127.0.0.1");
    check(t_listen(listener, 4) == 0, "listen() accepted the socket");

    long child = t_fork();
    if (child == 0) {
        t_close(listener);
        client();
    }
    check(child > 0, "forked a client");

    /* Blocking: nothing has connected yet, so this has to wait rather than
       answer EAGAIN. */
    peer.address = 0;
    peer.port = 0;
    int connection = t_accept_in(listener, &peer);
    check(connection >= 0, "accept() waited and returned a connection");
    check(peer.address == 0x0100007FU, "the peer is 127.0.0.1");
    check(peer.port != 0, "the peer has a port");

    uint8_t hello[4];
    check(read_fully(connection, hello, 4) == 4 && hello[0] == 'p' && hello[1] == 'i',
          "the server read what the client sent");
    check(write_fully(connection, (const uint8_t *)"pong", 4) == 4,
          "the server answered");

    static uint8_t received[BULK_BYTES];
    long got = read_fully(connection, received, BULK_BYTES);
    check(got == (long)BULK_BYTES, "64 KiB arrived through a 16 KiB window");

    int intact = 1;
    for (unsigned i = 0; i < BULK_BYTES && intact; i++)
        if (received[i] != (uint8_t)(i * 31U + (i >> 8))) intact = 0;
    check(intact, "every byte of it is what was sent");

    /* The client only exits 0 when its own half of the exchange worked. */
    int status = 0;
    while (t_waitpid(child, &status, 0) < 0) t_yield();
    check(((status >> 8) & 0xff) == 0, "the client finished happy");

    t_close(connection);
    t_close(listener);
}

static void run_refusal_test(void) {
    struct t_sockaddr_in nowhere;
    loopback_address(&nowhere, DEAD_PORT);
    int fd = t_socket(2, 1, 0);
    /* Nothing is listening there, so the stack must refuse rather than hang. */
    check(t_connect_in(fd, &nowhere) != 0, "connecting to a dead port failed");
    t_close(fd);
}

static void run_udp_test(void) {
    struct t_sockaddr_in address;
    loopback_address(&address, UDP_PORT);

    int fd = t_socket(2, 2 /* SOCK_DGRAM */, 0);
    check(fd >= 0, "opened a datagram socket");
    check(t_bind_in(fd, &address) == 0, "bound the datagram socket");
    check(t_connect_in(fd, &address) == 0, "pointed it at itself");
    check(t_write(fd, "loop", 4) == 4, "sent a datagram to 127.0.0.1");

    uint8_t buffer[8];
    long got = -11;
    for (unsigned attempt = 0; attempt < 1000 && got == -11; attempt++) {
        got = t_read(fd, buffer, sizeof(buffer));
        if (got == -11) t_yield();
    }
    check(got == 4 && buffer[0] == 'l' && buffer[3] == 'p',
          "the datagram came back");
    t_close(fd);
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    run_connection_tests();
    run_refusal_test();
    run_udp_test();

    if (failures) {
        say("TCPTEST: FAILED\n");
        return 1;
    }
    say("TCPTEST: PASS loopback and tcp servers work\n");
    return 0;
}
