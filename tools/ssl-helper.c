/*
 * ssl-helper: a minimal `openssl s_client` shim for Tunix, backed by mbedTLS.
 *
 * It implements the `openssl s_client` contract that HTTPS-capable clients use
 * to delegate TLS to an external helper:
 *
 *     openssl s_client -quiet -connect HOST:PORT [-servername NAME] \
 *             [-verify 100 -verify_return_error -verify_hostname NAME | -verify_ip IP]
 *
 * The helper inherits a socketpair on fd 0/1 (the *plaintext* side that the
 * caller reads and writes) and is expected to open its own TCP connection to
 * HOST:PORT, run the TLS handshake and then shuttle cleartext between fd 0/1
 * and the encrypted connection. Certificate verification is the helper's job:
 * with -verify_return_error a bad certificate must abort before any data flows.
 *
 * We install this program as /usr/bin/openssl so an execvp("openssl") from a
 * client finds it. It only understands `s_client`; anything else is rejected.
 * DNS works through getaddrinfo(), and TLS reuses the same mbedTLS setup as
 * https-get (SNI + verification against /etc/ssl/cert.pem).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

#ifndef TUNIX_MBEDTLS_VERSION
#define TUNIX_MBEDTLS_VERSION "unknown"
#endif

#define CA_BUNDLE "/etc/ssl/cert.pem"
#define RELAY_BUF 16384

/* Diagnostics go to stderr, which wget redirects to /dev/null; keep fd 1 clean
   for decrypted application data only. */
static void diag(const char *msg) { fprintf(stderr, "ssl-helper: %s\n", msg); }

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* mbedTLS BIO over a non-blocking socket fd. */
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n = send(fd, buf, len, 0);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}
static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n = recv(fd, buf, len, 0);
    if (n > 0) return (int)n;
    if (n == 0) return 0; /* peer closed at TCP layer */
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int is_ip_literal(const char *s) {
    struct in_addr a4;
    struct in6_addr a6;
    return inet_pton(AF_INET, s, &a4) == 1 || inet_pton(AF_INET6, s, &a6) == 1;
}

/* Resolve host and open a TCP connection. Returns fd or -1. */
static int tcp_connect(const char *host, const char *port) {
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Pump the non-blocking handshake to completion. Returns 0 on success. */
static int do_handshake(mbedtls_ssl_context *ssl, int sock) {
    for (;;) {
        int ret = mbedtls_ssl_handshake(ssl);
        if (ret == 0) return 0;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            struct pollfd pfd;
            pfd.fd = sock;
            pfd.events = (ret == MBEDTLS_ERR_SSL_WANT_WRITE) ? POLLOUT : POLLIN;
            pfd.revents = 0;
            if (poll(&pfd, 1, -1) < 0 && errno != EINTR) return -1;
            continue;
        }
        return -1;
    }
}

/* Full-duplex relay: fd 0/1 (plaintext) <-> TLS on sock. */
static int relay(mbedtls_ssl_context *ssl, int sock, int in_fd, int out_fd) {
    unsigned char c2s[RELAY_BUF]; size_t c2s_len = 0, c2s_off = 0; /* client -> server */
    unsigned char s2c[RELAY_BUF]; size_t s2c_len = 0, s2c_off = 0; /* server -> client */
    int in_open = 1, ssl_open = 1;

    for (;;) {
        /* Flush decrypted data to the client (fd 1). */
        while (s2c_off < s2c_len) {
            ssize_t w = write(out_fd, s2c + s2c_off, s2c_len - s2c_off);
            if (w > 0) { s2c_off += (size_t)w; continue; }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            return 0; /* client went away */
        }
        if (s2c_off == s2c_len) s2c_off = s2c_len = 0;

        /* Push buffered client bytes into the TLS connection. */
        while (c2s_off < c2s_len) {
            int r = mbedtls_ssl_write(ssl, c2s + c2s_off, c2s_len - c2s_off);
            if (r > 0) { c2s_off += (size_t)r; continue; }
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) break;
            return 1;
        }
        if (c2s_off == c2s_len) c2s_off = c2s_len = 0;

        /* Read decrypted server bytes when our outbound buffer is drained.
           TLS 1.3 servers commonly send one or more NewSessionTicket messages
           after the handshake; mbedTLS surfaces each as
           MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET rather than application
           data, so drain those before deciding the peer has nothing more. */
        if (ssl_open && s2c_len == 0) {
            int r;
            do {
                r = mbedtls_ssl_read(ssl, s2c, sizeof s2c);
            } while (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET);
            if (r > 0) s2c_len = (size_t)r;
            else if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) { /* wait */ }
            else ssl_open = 0; /* close_notify, EOF or error: no more app data */
        }

        /* Pull the next chunk of the client's request. On EOF the client
           (wget) has finished sending and half-closes its write side; we must
           NOT forward that as a TLS close_notify, or servers that react to it
           (e.g. Fastly) truncate the response mid-stream. Just stop reading and
           keep draining the server; the connection is torn down after relay(). */
        if (in_open && c2s_len == 0) {
            ssize_t rr = read(in_fd, c2s, sizeof c2s);
            if (rr > 0) c2s_len = (size_t)rr;
            else if (rr == 0) in_open = 0;
            else if (rr < 0 && errno != EAGAIN && errno != EWOULDBLOCK) in_open = 0;
        }

        /* Done once the server is finished and everything is flushed. */
        if (!ssl_open && s2c_len == 0) return 0;
        if (!in_open && !ssl_open) return 0;

        struct pollfd pfd[3];
        nfds_t n = 0;
        pfd[n].fd = sock;
        pfd[n].events = POLLIN | (c2s_off < c2s_len ? POLLOUT : 0);
        pfd[n].revents = 0; n++;
        if (in_open && c2s_len == 0) { pfd[n].fd = in_fd; pfd[n].events = POLLIN; pfd[n].revents = 0; n++; }
        if (s2c_off < s2c_len) { pfd[n].fd = out_fd; pfd[n].events = POLLOUT; pfd[n].revents = 0; n++; }
        if (poll(pfd, n, -1) < 0 && errno != EINTR) return 1;
    }
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "s_client") != 0) {
        fprintf(stderr, "ssl-helper (mbedTLS %s): only 'openssl s_client' is supported\n",
                TUNIX_MBEDTLS_VERSION);
        return 2;
    }

    const char *connect_to = NULL; /* HOST:PORT */
    const char *servername = NULL;
    const char *verify_name = NULL;
    int verify = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-connect") && i + 1 < argc) connect_to = argv[++i];
        else if (!strcmp(argv[i], "-servername") && i + 1 < argc) servername = argv[++i];
        else if (!strcmp(argv[i], "-verify_hostname") && i + 1 < argc) { verify_name = argv[++i]; verify = 1; }
        else if (!strcmp(argv[i], "-verify_ip") && i + 1 < argc) { verify_name = argv[++i]; verify = 1; }
        else if (!strcmp(argv[i], "-verify_return_error")) verify = 1;
        else if (!strcmp(argv[i], "-verify") && i + 1 < argc) i++; /* skip depth */
        /* -quiet and anything else: ignored */
    }
    if (!connect_to) { diag("missing -connect HOST:PORT"); return 2; }

    char host[256];
    const char *port = "443";
    const char *colon = strrchr(connect_to, ':');
    if (colon && colon != connect_to) {
        size_t hl = (size_t)(colon - connect_to);
        if (hl >= sizeof host) hl = sizeof host - 1;
        memcpy(host, connect_to, hl);
        host[hl] = 0;
        port = colon + 1;
    } else {
        snprintf(host, sizeof host, "%s", connect_to);
    }

    /* SNI / verification hostname: prefer -servername, else the connect host,
       but never send an IP literal as SNI (RFC 6066). */
    const char *sni = servername ? servername : host;
    if (is_ip_literal(sni)) sni = verify_name; /* may be NULL -> no SNI */

    int sock = tcp_connect(host, port);
    if (sock < 0) { diag("connect failed"); return 1; }
    if (set_nonblock(sock) != 0) { diag("nonblock sock failed"); return 1; }
    if (set_nonblock(0) != 0 || set_nonblock(1) != 0) { diag("nonblock stdio failed"); return 1; }

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt cacert;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);
    mbedtls_x509_crt_init(&cacert);

    const char *pers = "tunix-ssl-helper";
    int rc = 1;
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        diag("RNG seed failed");
        goto out;
    }
    if (verify) {
        if (mbedtls_x509_crt_parse_file(&cacert, CA_BUNDLE) < 0) {
            diag("could not load CA bundle " CA_BUNDLE);
            goto out;
        }
    }
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        diag("ssl config failed");
        goto out;
    }
    mbedtls_ssl_conf_authmode(&conf, verify ? MBEDTLS_SSL_VERIFY_REQUIRED
                                            : MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    if (mbedtls_ssl_setup(&ssl, &conf) != 0) { diag("ssl setup failed"); goto out; }
    if (sni && mbedtls_ssl_set_hostname(&ssl, sni) != 0) { diag("set hostname failed"); goto out; }
    mbedtls_ssl_set_bio(&ssl, &sock, bio_send, bio_recv, NULL);

    if (do_handshake(&ssl, sock) != 0) { diag("TLS handshake failed"); goto out; }
    if (verify && mbedtls_ssl_get_verify_result(&ssl) != 0) {
        diag("certificate verification failed");
        goto out; /* -verify_return_error: abort before relaying any data */
    }

    rc = relay(&ssl, sock, 0, 1);
    mbedtls_ssl_close_notify(&ssl);

out:
    close(sock);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&cacert);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return rc;
}
