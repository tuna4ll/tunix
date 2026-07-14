/*
 * https-get: a small HTTPS client for Tunix.
 *
 * It exercises the Tunix TCP stack under TLS: resolve a host over DNS, open a
 * TCP connection, run an mbedTLS handshake (SNI + certificate verification
 * against /etc/ssl/cert.pem), send an HTTP/1.1 GET and stream the response.
 *
 * DNS uses res_query() rather than getaddrinfo(): on Tunix the low-level
 * resolver works but the getaddrinfo() wrapper does not, so we parse the A
 * record ourselves. TLS I/O is wired to our own socket via a custom BIO so we
 * never touch mbedtls_net_connect() (which would call getaddrinfo()).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <resolv.h>

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

#ifndef TUNIX_MBEDTLS_VERSION
#define TUNIX_MBEDTLS_VERSION "unknown"
#endif

#define CA_BUNDLE "/etc/ssl/cert.pem"

static void die(const char *msg) { fprintf(stderr, "https-get: %s\n", msg); exit(1); }

/* Parse the first A record out of a res_query() answer. Returns 0 on success
   and stores the IPv4 address in network byte order. */
static int dns_resolve_a(const char *host, uint32_t *out_ip) {
    unsigned char ans[1024];
    int n = res_query(host, 1 /*C_IN*/, 1 /*T_A*/, ans, sizeof ans);
    if (n < 12) return -1;
    int qd = (ans[4] << 8) | ans[5];
    int an = (ans[6] << 8) | ans[7];
    const unsigned char *p = ans + 12;
    const unsigned char *end = ans + n;
    for (int i = 0; i < qd && p < end; i++) {           /* skip questions */
        while (p < end && *p) p += *p + 1;
        p += 1 + 4;                                     /* null + qtype/qclass */
    }
    for (int i = 0; i < an && p + 10 <= end; i++) {     /* walk answers */
        if ((*p & 0xc0) == 0xc0) p += 2;                /* compressed name */
        else { while (p < end && *p) p += *p + 1; p += 1; }
        if (p + 10 > end) break;
        int type = (p[0] << 8) | p[1];
        int rdlen = (p[8] << 8) | p[9];
        const unsigned char *rd = p + 10;
        if (rd + rdlen > end) break;
        if (type == 1 && rdlen == 4) { memcpy(out_ip, rd, 4); return 0; }
        p = rd + rdlen;
    }
    return -1;
}

/* Blocking TCP connect to ip:port (ip in network byte order). */
static int tcp_connect(uint32_t ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = ip;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    return fd;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    long n = send(fd, buf, len, 0);
    if (n < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)n;
}
static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    long n = recv(fd, buf, len, 0);
    if (n < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)n;
}

int main(int argc, char **argv) {
    int insecure = 0, resolve_only = 0;
    const char *url = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--insecure")) insecure = 1;
        else if (!strcmp(argv[i], "--resolve-only")) resolve_only = 1;
        else url = argv[i];
    }
    if (!url) {
        fprintf(stderr, "usage: https-get [--insecure] [--resolve-only] https://host[:port]/path\n");
        fprintf(stderr, "https-get (mbedTLS %s)\n", TUNIX_MBEDTLS_VERSION);
        return 2;
    }

    /* Parse URL: [https://]host[:port][/path] */
    const char *s = url;
    if (!strncmp(s, "https://", 8)) s += 8;
    else if (!strncmp(s, "http://", 7)) s += 7;
    char host[256]; char path[1024]; uint16_t port = 443;
    size_t hl = 0;
    while (*s && *s != ':' && *s != '/' && hl < sizeof host - 1) host[hl++] = *s++;
    host[hl] = 0;
    if (*s == ':') { port = (uint16_t)atoi(s + 1); while (*s && *s != '/') s++; }
    snprintf(path, sizeof path, "%s", (*s == '/') ? s : "/");
    if (!host[0]) die("could not parse host from URL");

    res_init();
    uint32_t ip = 0;
    if (dns_resolve_a(host, &ip) != 0) die("DNS resolution failed");
    char ipstr[32];
    inet_ntop(AF_INET, &ip, ipstr, sizeof ipstr);
    fprintf(stderr, "https-get: %s -> %s port %u\n", host, ipstr, port);
    if (resolve_only) return 0;

    int fd = tcp_connect(ip, port);
    if (fd < 0) die("TCP connect failed");

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

    const char *pers = "tunix-https-get";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0)
        die("RNG seed failed");

    if (!insecure) {
        int r = mbedtls_x509_crt_parse_file(&cacert, CA_BUNDLE);
        if (r < 0) die("could not load CA bundle " CA_BUNDLE);
    }

    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        die("ssl config failed");
    mbedtls_ssl_conf_authmode(&conf, insecure ? MBEDTLS_SSL_VERIFY_NONE
                                              : MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    if (mbedtls_ssl_setup(&ssl, &conf) != 0) die("ssl setup failed");
    if (mbedtls_ssl_set_hostname(&ssl, host) != 0) die("set hostname failed");
    mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);

    int ret;
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char eb[128]; mbedtls_strerror(ret, eb, sizeof eb);
            fprintf(stderr, "https-get: TLS handshake failed: -0x%04x %s\n", (unsigned)-ret, eb);
            return 1;
        }
    }
    fprintf(stderr, "https-get: handshake OK, %s / %s\n",
            mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl));

    uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
    if (flags == 0) fprintf(stderr, "https-get: certificate verify OK\n");
    else {
        char vb[512]; mbedtls_x509_crt_verify_info(vb, sizeof vb, "  ", flags);
        fprintf(stderr, "https-get: certificate verify FAILED:\n%s", vb);
        if (!insecure) return 1;
    }

    char req[1400];
    int rn = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: tunix-https-get\r\nConnection: close\r\n\r\n",
        path, host);
    for (int off = 0; off < rn; ) {
        ret = mbedtls_ssl_write(&ssl, (const unsigned char *)req + off, (size_t)(rn - off));
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret <= 0) die("TLS write failed");
        off += ret;
    }

    for (;;) {
        unsigned char buf[4096];
        ret = mbedtls_ssl_read(&ssl, buf, sizeof buf);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) break;
        if (ret < 0) { fprintf(stderr, "https-get: TLS read error\n"); break; }
        fwrite(buf, 1, (size_t)ret, stdout);
    }
    fflush(stdout);

    mbedtls_ssl_close_notify(&ssl);
    close(fd);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&cacert);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return 0;
}
