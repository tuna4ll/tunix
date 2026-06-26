#include <stddef.h>
#include <stdint.h>
#include "include/io.h"
#include "include/kstring.h"
#include "include/random.h"
#include "include/spinlock.h"
#include "include/time.h"

#define RANDOM_RESEED_INTERVAL (1024U * 1024U)

struct sha256_ctx {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_length;
};

static spinlock_t random_lock;
static uint32_t chacha_state[16];
static uint64_t generated_bytes;
static int seeded;

static const uint32_t sha256_k[64] = {
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
};

static inline uint32_t rotr32(uint32_t value, unsigned count) {
    count &= 31U;
    return count ? (value >> count) | (value << (32U - count)) : value;
}

static inline uint32_t load32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void store32_le(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static inline uint32_t load32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void store32_be(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void sha256_transform(struct sha256_ctx *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++) w[i] = load32_be(block + i * 4U);
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t temp1 = h + s1 + choice + sha256_k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + majority;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(struct sha256_ctx *ctx) {
    static const uint32_t initial[8] = {
        0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
        0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U
    };
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->bit_count = 0;
    ctx->block_length = 0;
}

static void sha256_update(struct sha256_ctx *ctx, const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    while (length) {
        size_t room = 64U - ctx->block_length;
        size_t take = length < room ? length : room;
        memcpy(ctx->block + ctx->block_length, bytes, take);
        ctx->block_length += take;
        bytes += take;
        length -= take;
        if (ctx->block_length == 64U) {
            sha256_transform(ctx, ctx->block);
            ctx->bit_count += 512U;
            ctx->block_length = 0;
        }
    }
}

static void sha256_final(struct sha256_ctx *ctx, uint8_t digest[32]) {
    ctx->bit_count += (uint64_t)ctx->block_length * 8ULL;
    ctx->block[ctx->block_length++] = 0x80U;
    if (ctx->block_length > 56U) {
        while (ctx->block_length < 64U) ctx->block[ctx->block_length++] = 0;
        sha256_transform(ctx, ctx->block);
        ctx->block_length = 0;
    }
    while (ctx->block_length < 56U) ctx->block[ctx->block_length++] = 0;
    for (unsigned i = 0; i < 8; i++)
        ctx->block[63U - i] = (uint8_t)(ctx->bit_count >> (i * 8U));
    sha256_transform(ctx, ctx->block);
    for (unsigned i = 0; i < 8; i++) store32_be(digest + i * 4U, ctx->state[i]);
}

static inline uint64_t read_tsc(void) {
    uint32_t low, high;
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) : : "memory");
    return ((uint64_t)high << 32) | low;
}

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

static int cpu_has_rdrand(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    return (c & (1U << 30)) != 0;
}

static int cpu_has_rdseed(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a < 7U) return 0;
    cpuid(7, 0, &a, &b, &c, &d);
    return (b & (1U << 18)) != 0;
}

static int get_rdrand(uint64_t *value) {
    uint64_t result;
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(result), "=m"(ok) : : "cc");
    if (ok) *value = result;
    return ok != 0;
}

static int get_rdseed(uint64_t *value) {
    uint64_t result;
    unsigned char ok;
    __asm__ volatile("rdseed %0; setc %1" : "=r"(result), "=m"(ok) : : "cc");
    if (ok) *value = result;
    return ok != 0;
}

static size_t collect_entropy(uint8_t *output, size_t capacity) {
    uint64_t values[64];
    size_t count = 0;
    uint32_t a, b, c, d;

    cpuid(0, 0, &a, &b, &c, &d);
    values[count++] = ((uint64_t)a << 32) | b;
    values[count++] = ((uint64_t)c << 32) | d;
    values[count++] = time_realtime_ns();
    values[count++] = time_uptime_ns();
    values[count++] = time_tsc_frequency();
    values[count++] = (uint64_t)(uintptr_t)&values;
    values[count++] = (uint64_t)(uintptr_t)&chacha_state;

    if (cpu_has_rdseed()) {
        for (unsigned i = 0; i < 8 && count < 64U; i++) {
            uint64_t value;
            for (unsigned retry = 0; retry < 32U; retry++) {
                if (get_rdseed(&value)) { values[count++] = value; break; }
                __asm__ volatile("pause");
            }
        }
    }
    if (cpu_has_rdrand()) {
        for (unsigned i = 0; i < 8 && count < 64U; i++) {
            uint64_t value;
            for (unsigned retry = 0; retry < 16U; retry++) {
                if (get_rdrand(&value)) { values[count++] = value; break; }
                __asm__ volatile("pause");
            }
        }
    }

    uint64_t previous = read_tsc();
    while (count < 64U) {
        uint64_t accumulator = 0;
        for (unsigned sample = 0; sample < 32U; sample++) {
            unsigned loops = 1U + (unsigned)(previous & 0x3FU);
            for (unsigned i = 0; i < loops; i++) {
                accumulator ^= (uint64_t)inb(0x61U) << ((i & 7U) * 8U);
                __asm__ volatile("pause");
            }
            uint64_t now = read_tsc();
            accumulator ^= rotr32((uint32_t)(now - previous), sample & 31U);
            accumulator ^= now;
            previous = now;
        }
        values[count++] = accumulator ^ time_uptime_ns();
    }

    size_t bytes = count * sizeof(values[0]);
    if (bytes > capacity) bytes = capacity;
    memcpy(output, values, bytes);
    memset(values, 0, sizeof(values));
    return bytes;
}

#define QR(a,b,c,d) do { \
    a += b; d ^= a; d = (d << 16) | (d >> 16); \
    c += d; b ^= c; b = (b << 12) | (b >> 20); \
    a += b; d ^= a; d = (d << 8) | (d >> 24); \
    c += d; b ^= c; b = (b << 7) | (b >> 25); \
} while (0)

static void chacha_block(uint8_t output[64]) {
    uint32_t working[16];
    memcpy(working, chacha_state, sizeof(working));
    for (unsigned i = 0; i < 10U; i++) {
        QR(working[0], working[4], working[8], working[12]);
        QR(working[1], working[5], working[9], working[13]);
        QR(working[2], working[6], working[10], working[14]);
        QR(working[3], working[7], working[11], working[15]);
        QR(working[0], working[5], working[10], working[15]);
        QR(working[1], working[6], working[11], working[12]);
        QR(working[2], working[7], working[8], working[13]);
        QR(working[3], working[4], working[9], working[14]);
    }
    for (unsigned i = 0; i < 16U; i++) store32_le(output + i * 4U, working[i] + chacha_state[i]);
    if (++chacha_state[12] == 0) chacha_state[13]++;
    memset(working, 0, sizeof(working));
}

static void install_seed(const uint8_t digest[32], const uint8_t nonce[32]) {
    chacha_state[0] = 0x61707865U;
    chacha_state[1] = 0x3320646eU;
    chacha_state[2] = 0x79622d32U;
    chacha_state[3] = 0x6b206574U;
    for (unsigned i = 0; i < 8U; i++) chacha_state[4U + i] = load32_le(digest + i * 4U);
    chacha_state[12] = load32_le(nonce);
    chacha_state[13] = load32_le(nonce + 4U);
    chacha_state[14] = load32_le(nonce + 8U);
    chacha_state[15] = load32_le(nonce + 12U);
}

static void reseed_locked(const void *extra, size_t extra_length) {
    uint8_t entropy[512];
    uint8_t digest[32];
    uint8_t nonce[32];
    struct sha256_ctx ctx;
    size_t entropy_length = collect_entropy(entropy, sizeof(entropy));

    sha256_init(&ctx);
    sha256_update(&ctx, chacha_state, sizeof(chacha_state));
    sha256_update(&ctx, entropy, entropy_length);
    if (extra && extra_length) sha256_update(&ctx, extra, extra_length);
    sha256_final(&ctx, digest);

    static const uint8_t domain[] = "Tunix ChaCha20 DRBG nonce";
    sha256_init(&ctx);
    sha256_update(&ctx, digest, sizeof(digest));
    sha256_update(&ctx, domain, sizeof(domain));
    sha256_update(&ctx, entropy, entropy_length);
    sha256_final(&ctx, nonce);

    install_seed(digest, nonce);
    generated_bytes = 0;
    seeded = 1;
    memset(entropy, 0, sizeof(entropy));
    memset(digest, 0, sizeof(digest));
    memset(nonce, 0, sizeof(nonce));
    memset(&ctx, 0, sizeof(ctx));
}

void random_init(void) {
    spinlock_init(&random_lock);
    memset(chacha_state, 0, sizeof(chacha_state));
    spinlock_acquire(&random_lock);
    reseed_locked(NULL, 0);
    spinlock_release(&random_lock);
}

void random_mix(const void *buffer, size_t length) {
    if (!buffer || !length) return;
    spinlock_acquire(&random_lock);
    reseed_locked(buffer, length);
    spinlock_release(&random_lock);
}

void random_get_bytes(void *buffer, size_t length) {
    if (!buffer || !length) return;
    uint8_t *output = (uint8_t *)buffer;
    uint8_t block[64];

    spinlock_acquire(&random_lock);
    if (!seeded || generated_bytes >= RANDOM_RESEED_INTERVAL) reseed_locked(NULL, 0);
    while (length) {
        chacha_block(block);
        size_t take = length < sizeof(block) ? length : sizeof(block);
        memcpy(output, block, take);
        output += take;
        length -= take;
        generated_bytes += take;
    }

    chacha_block(block);
    for (unsigned i = 0; i < 8U; i++) chacha_state[4U + i] = load32_le(block + i * 4U);
    chacha_state[14] ^= load32_le(block + 32U);
    chacha_state[15] ^= load32_le(block + 36U);
    memset(block, 0, sizeof(block));
    spinlock_release(&random_lock);
}

int random_is_seeded(void) {
    return seeded;
}
