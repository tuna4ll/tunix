#include <stddef.h>
#include <stdint.h>

#include "../../include/block.h"
#include "../../include/cpu.h"
#include "../../include/sdhci.h"
#include "../../include/time.h"

extern void kprintf(const char *fmt, ...);

#define REG_BLOCK 0x04U
#define REG_ARGUMENT 0x08U
#define REG_COMMAND 0x0CU
#define REG_RESPONSE 0x10U
#define REG_BUFFER 0x20U
#define REG_PRESENT 0x24U
#define REG_HOST 0x28U
#define REG_CLOCK 0x2CU
#define REG_STATUS 0x30U
#define REG_STATUS_ENABLE 0x34U
#define REG_SIGNAL_ENABLE 0x38U
#define REG_CAPABILITIES 0x40U
#define REG_VERSION 0xFCU

#define PRESENT_COMMAND_INHIBIT (1U << 0)
#define PRESENT_DATA_INHIBIT (1U << 1)

#define STATUS_COMMAND_DONE (1U << 0)
#define STATUS_TRANSFER_DONE (1U << 1)
#define STATUS_WRITE_READY (1U << 4)
#define STATUS_READ_READY (1U << 5)
#define STATUS_CARD_INTERRUPT (1U << 8)
#define STATUS_ERROR (1U << 15)

#define CLOCK_INTERNAL_ENABLE (1U << 0)
#define CLOCK_INTERNAL_STABLE (1U << 1)
#define CLOCK_CARD_ENABLE (1U << 2)

#define RESET_ALL (1U << 24)
#define RESET_COMMAND (1U << 25)
#define RESET_DATA (1U << 26)

#define RESPONSE_NONE 0U
#define RESPONSE_136 1U
#define RESPONSE_48 2U
#define RESPONSE_48_BUSY 3U
#define CHECK_CRC 0x08U
#define CHECK_INDEX 0x10U
#define DATA_PRESENT 0x20U

#define R1 (RESPONSE_48 | CHECK_CRC | CHECK_INDEX)
#define R1B (RESPONSE_48_BUSY | CHECK_CRC | CHECK_INDEX)
#define R2 (RESPONSE_136 | CHECK_CRC)
#define R3 RESPONSE_48

#define TRANSFER_BLOCK_COUNT 0x02U
#define TRANSFER_AUTO_CMD12 0x04U
#define TRANSFER_READ 0x10U
#define TRANSFER_MULTIPLE 0x20U

#define SECTOR 512U
#define CHUNK_BLOCKS 64U
#define MAX_HOSTS 4U

#define COMMAND_TIMEOUT_NS 1000000000ULL
#define DATA_TIMEOUT_NS 5000000000ULL

struct sdhci_host {
    uint64_t base;
    uint64_t clock_hz;
    int quirks;
    uint32_t rca;
    int high_capacity;
    uint64_t sectors;
};

static struct sdhci_host hosts[MAX_HOSTS];
static unsigned host_count;

static void settle_ns(uint64_t nanoseconds) {
    uint64_t deadline = time_uptime_ns() + nanoseconds;
    while (time_uptime_ns() < deadline) cpu_relax();
}

static uint32_t read_register(const struct sdhci_host *host, uint32_t offset) {
    return *(volatile uint32_t *)(host->base + offset);
}

static void write_register(const struct sdhci_host *host, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(host->base + offset) = value;
    if (host->quirks & SDHCI_QUIRK_WRITE_DELAY) settle_ns(20000ULL);
}

static int wait_bits(const struct sdhci_host *host, uint32_t offset, uint32_t mask,
                     uint32_t wanted, uint64_t timeout_ns) {
    uint64_t deadline = time_uptime_ns() + timeout_ns;
    while ((read_register(host, offset) & mask) != wanted) {
        if (time_uptime_ns() >= deadline) return -1;
        cpu_relax();
    }
    return 0;
}

static int reset(const struct sdhci_host *host, uint32_t which) {
    write_register(host, REG_CLOCK, read_register(host, REG_CLOCK) | which);
    return wait_bits(host, REG_CLOCK, which, 0, 100000000ULL);
}

static int set_clock(struct sdhci_host *host, uint64_t target) {
    uint32_t clock = read_register(host, REG_CLOCK) & ~0xFFFFU;
    write_register(host, REG_CLOCK, clock);

    uint32_t version = (read_register(host, REG_VERSION) >> 16) & 0xFFU;
    uint32_t divider;
    if (version >= 2U) {
        uint64_t steps = 0;
        if (host->clock_hz > target) {
            steps = (host->clock_hz + 2U * target - 1U) / (2U * target);
            if (steps > 1023U) steps = 1023U;
        }
        divider = (uint32_t)((steps & 0xFFU) << 8) | (uint32_t)(((steps >> 8) & 3U) << 6);
    } else {
        uint32_t steps = 1;
        while (steps < 256U && host->clock_hz / steps > target) steps <<= 1;
        divider = (steps >> 1) << 8;
    }

    write_register(host, REG_CLOCK, clock | divider | CLOCK_INTERNAL_ENABLE);
    if (wait_bits(host, REG_CLOCK, CLOCK_INTERNAL_STABLE, CLOCK_INTERNAL_STABLE,
                  50000000ULL) != 0) return -1;
    write_register(host, REG_CLOCK, read_register(host, REG_CLOCK) | CLOCK_CARD_ENABLE);
    settle_ns(2000000ULL);
    return 0;
}

static int command(struct sdhci_host *host, uint32_t index, uint32_t argument,
                   uint32_t flags, uint32_t transfer, uint32_t response[4]) {
    uint32_t inhibit = PRESENT_COMMAND_INHIBIT;
    if ((flags & DATA_PRESENT) || (flags & 3U) == RESPONSE_48_BUSY) inhibit |= PRESENT_DATA_INHIBIT;
    if (wait_bits(host, REG_PRESENT, inhibit, 0, COMMAND_TIMEOUT_NS) != 0) return -1;

    write_register(host, REG_STATUS, 0xFFFFFFFFU);
    write_register(host, REG_ARGUMENT, argument);
    write_register(host, REG_COMMAND, ((index << 8 | flags) << 16) | transfer);

    uint64_t deadline = time_uptime_ns() + COMMAND_TIMEOUT_NS;
    uint32_t status;
    for (;;) {
        status = read_register(host, REG_STATUS);
        if (status & (STATUS_COMMAND_DONE | STATUS_ERROR)) break;
        if (time_uptime_ns() >= deadline) {
            reset(host, RESET_COMMAND | RESET_DATA);
            return -1;
        }
        cpu_relax();
    }
    if (status & STATUS_ERROR) {
        write_register(host, REG_STATUS, status);
        reset(host, RESET_COMMAND | RESET_DATA);
        return -1;
    }
    write_register(host, REG_STATUS, STATUS_COMMAND_DONE);

    if (response)
        for (unsigned word = 0; word < 4U; word++)
            response[word] = read_register(host, REG_RESPONSE + word * 4U);

    if ((flags & 3U) == RESPONSE_48_BUSY && !(flags & DATA_PRESENT)) {
        if (wait_bits(host, REG_STATUS, STATUS_TRANSFER_DONE | STATUS_ERROR, STATUS_TRANSFER_DONE,
                      DATA_TIMEOUT_NS) != 0) {
            write_register(host, REG_STATUS, 0xFFFFFFFFU);
            return -1;
        }
        write_register(host, REG_STATUS, STATUS_TRANSFER_DONE);
    }
    return 0;
}

static int application_command(struct sdhci_host *host, uint32_t index, uint32_t argument,
                                uint32_t flags, uint32_t response[4]) {
    uint32_t ignored[4];
    if (command(host, 55, host->rca << 16, R1, 0, ignored) != 0) return -1;
    return command(host, index, argument, flags, 0, response);
}

static uint64_t csd_sectors(const uint32_t csd[4]) {
    uint32_t structure = (csd[3] >> 22) & 3U;
    if (structure == 1U) {
        uint64_t size = (csd[1] >> 8) & 0x3FFFFFU;
        return (size + 1U) * 1024U;
    }
    uint32_t block_length = (csd[2] >> 8) & 0xFU;
    uint64_t size = ((csd[1] >> 22) & 0x3FFU) | ((uint64_t)(csd[2] & 3U) << 10);
    uint32_t multiplier = (csd[1] >> 7) & 7U;
    uint64_t bytes = (size + 1U) << (multiplier + 2U + block_length);
    return bytes / SECTOR;
}

static int card_init(struct sdhci_host *host) {
    if (reset(host, RESET_ALL) != 0) return -1;
    uint32_t host_control = read_register(host, REG_HOST) & ~0xFF00U;
    write_register(host, REG_HOST, host_control | (0x0EU << 8));
    write_register(host, REG_HOST, host_control | (0x0FU << 8));
    settle_ns(10000000ULL);
    write_register(host, REG_STATUS_ENABLE, 0xFFFFFFFFU & ~STATUS_CARD_INTERRUPT);
    write_register(host, REG_SIGNAL_ENABLE, 0);
    write_register(host, REG_CLOCK, (read_register(host, REG_CLOCK) & ~(0xFFU << 16)) | (0x0EU << 16));
    if (set_clock(host, 400000ULL) != 0) return -2;

    uint32_t response[4];
    command(host, 0, 0, RESPONSE_NONE, 0, NULL);
    int version2 = command(host, 8, 0x1AAU, R1, 0, response) == 0 && (response[0] & 0xFFFU) == 0x1AAU;

    uint64_t deadline = time_uptime_ns() + 2000000000ULL;
    host->rca = 0;
    for (;;) {
        if (application_command(host, 41, version2 ? 0x40FF8000U : 0x00FF8000U, R3, response) != 0)
            return -3;
        if (response[0] & 0x80000000U) break;
        if (time_uptime_ns() >= deadline) return -4;
        settle_ns(10000000ULL);
    }
    host->high_capacity = (response[0] & 0x40000000U) != 0;

    if (command(host, 2, 0, R2, 0, response) != 0) return -5;
    if (command(host, 3, 0, R1, 0, response) != 0) return -6;
    host->rca = response[0] >> 16;
    if (command(host, 9, host->rca << 16, R2, 0, response) != 0) return -7;
    host->sectors = csd_sectors(response);
    if (command(host, 7, host->rca << 16, R1B, 0, response) != 0) return -8;
    if (!host->high_capacity && command(host, 16, SECTOR, R1, 0, response) != 0) return -9;
    if (set_clock(host, 25000000ULL) != 0) return -10;
    return 0;
}

static int transfer_chunk(struct sdhci_host *host, uint64_t lba, uint32_t count,
                          uint8_t *buffer, int write) {
    uint32_t argument = host->high_capacity ? (uint32_t)lba : (uint32_t)(lba * SECTOR);
    uint32_t mode = TRANSFER_BLOCK_COUNT | (write ? 0 : TRANSFER_READ);
    uint32_t index = write ? 24U : 17U;
    if (count > 1U) {
        mode |= TRANSFER_MULTIPLE | TRANSFER_AUTO_CMD12;
        index = write ? 25U : 18U;
    }
    write_register(host, REG_BLOCK, (count << 16) | SECTOR);
    if (command(host, index, argument, R1 | DATA_PRESENT, mode, NULL) != 0) return -1;

    uint32_t ready = write ? STATUS_WRITE_READY : STATUS_READ_READY;
    for (uint32_t block = 0; block < count; block++) {
        if (wait_bits(host, REG_STATUS, ready | STATUS_ERROR, ready, DATA_TIMEOUT_NS) != 0) {
            write_register(host, REG_STATUS, 0xFFFFFFFFU);
            reset(host, RESET_COMMAND | RESET_DATA);
            return -1;
        }
        write_register(host, REG_STATUS, ready);
        uint8_t *sector = buffer + (uint64_t)block * SECTOR;
        for (unsigned offset = 0; offset < SECTOR; offset += 4U) {
            if (write) {
                uint32_t word = (uint32_t)sector[offset] | ((uint32_t)sector[offset + 1] << 8) |
                                ((uint32_t)sector[offset + 2] << 16) | ((uint32_t)sector[offset + 3] << 24);
                *(volatile uint32_t *)(host->base + REG_BUFFER) = word;
            } else {
                uint32_t word = *(volatile uint32_t *)(host->base + REG_BUFFER);
                sector[offset] = (uint8_t)word;
                sector[offset + 1] = (uint8_t)(word >> 8);
                sector[offset + 2] = (uint8_t)(word >> 16);
                sector[offset + 3] = (uint8_t)(word >> 24);
            }
        }
    }
    if (wait_bits(host, REG_STATUS, STATUS_TRANSFER_DONE | STATUS_ERROR, STATUS_TRANSFER_DONE,
                  DATA_TIMEOUT_NS) != 0) {
        write_register(host, REG_STATUS, 0xFFFFFFFFU);
        reset(host, RESET_COMMAND | RESET_DATA);
        return -1;
    }
    write_register(host, REG_STATUS, STATUS_TRANSFER_DONE);
    return 0;
}

static int sdhci_transfer(void *context, uint64_t lba, uint32_t count, uint8_t *buffer, int write) {
    struct sdhci_host *host = context;
    if (lba >= host->sectors || count > host->sectors - lba) return -1;
    while (count) {
        uint32_t chunk = count < CHUNK_BLOCKS ? count : CHUNK_BLOCKS;
        int status = -1;
        for (unsigned attempt = 0; attempt < 3U && status != 0; attempt++)
            status = transfer_chunk(host, lba, chunk, buffer, write);
        if (status != 0) return -1;
        lba += chunk;
        count -= chunk;
        buffer += (uint64_t)chunk * SECTOR;
    }
    return 0;
}

static int sdhci_read(void *context, uint64_t lba, uint32_t count, void *destination) {
    return sdhci_transfer(context, lba, count, destination, 0);
}

static int sdhci_write(void *context, uint64_t lba, uint32_t count, const void *source) {
    return sdhci_transfer(context, lba, count, (uint8_t *)(uintptr_t)source, 1);
}

int sdhci_attach(uint64_t registers, uint64_t clock_hz, int quirks) {
    if (!registers || host_count >= MAX_HOSTS) return -1;
    struct sdhci_host *host = &hosts[host_count];
    host->base = registers;
    host->quirks = quirks;
    uint64_t capability_clock = ((read_register(host, REG_CAPABILITIES) >> 8) & 0xFFU) * 1000000ULL;
    host->clock_hz = capability_clock ? capability_clock : (clock_hz ? clock_hz : 100000000ULL);

    int status = card_init(host);
    if (status != 0) {
        kprintf("SDHCI: no card (%d)\n", status);
        return -1;
    }

    struct block_device device = {
        .name = "sdhci0",
        .sectors = host->sectors,
        .read = sdhci_read,
        .write = sdhci_write,
        .flush = NULL,
        .context = host,
    };
    device.name[5] = (char)('0' + host_count);
    host_count++;
    if (block_register(&device) < 0) return -1;
    kprintf("SDHCI: %s card, %u MiB\n", host->high_capacity ? "high-capacity" : "standard",
            (unsigned)(host->sectors / 2048U));
    return 0;
}
