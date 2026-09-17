#include <stdint.h>

#include "../../include/cpu.h"
#include "../../include/dma.h"
#include "../../include/vmm.h"
#include "aarch64.h"

extern void kprintf(const char *fmt, ...);

#define GITS_CTLR 0x0000U
#define GITS_TYPER 0x0008U
#define GITS_CBASER 0x0080U
#define GITS_CWRITER 0x0088U
#define GITS_CREADR 0x0090U
#define GITS_BASER 0x0100U
#define GITS_TRANSLATER 0x10040ULL
#define GITS_BYTES 0x20000ULL

#define GICR_CTLR 0x0000U
#define GICR_TYPER 0x0008U
#define GICR_PROPBASER 0x0070U
#define GICR_PENDBASER 0x0078U

#define TABLE_VALID (1ULL << 63)
#define TABLE_TYPE_DEVICE 1U
#define TABLE_TYPE_COLLECTION 4U
#define PENDING_ZEROED (1ULL << 62)

#define COMMAND_BYTES 32U
#define COMMAND_QUEUE_BYTES 4096ULL
#define COMMAND_MAPD 0x08ULL
#define COMMAND_MAPC 0x09ULL
#define COMMAND_MAPTI 0x0AULL
#define COMMAND_INVALL 0x0DULL

#define LPI_FIRST 8192U
#define LPI_ID_BITS 14U
#define LPI_COUNT 8192U
#define LPI_ENABLED_PRIORITY 0xA1U
#define EVENT_BITS 4U
#define MAX_DEVICE_BITS 16U
#define MAX_DEVICES 32U

static uint64_t its;
static uint64_t command_queue;
static uint64_t command_offset;
static unsigned itt_entry_bytes;
static unsigned device_bits;
static uint8_t *lpi_properties;
static uint32_t devices[MAX_DEVICES];
static unsigned device_count;
static int ready;

static uint32_t read32(uint64_t base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

static void write32(uint64_t base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(base + offset) = value;
}

static uint64_t read64(uint64_t base, uint32_t offset) {
    return *(volatile uint64_t *)(base + offset);
}

static void write64(uint64_t base, uint32_t offset, uint64_t value) {
    *(volatile uint64_t *)(base + offset) = value;
}

static int send_command(uint64_t first, uint64_t second, uint64_t third) {
    uint64_t *slot = (uint64_t *)(command_queue + command_offset);
    slot[0] = first;
    slot[1] = second;
    slot[2] = third;
    slot[3] = 0;
    command_offset = (command_offset + COMMAND_BYTES) % COMMAND_QUEUE_BYTES;
    write64(its, GITS_CWRITER, command_offset);
    for (unsigned spin = 0; spin < 10000000U; spin++) {
        uint64_t done = read64(its, GITS_CREADR);
        if (done & 1ULL) {
            kprintf("ITS: command %x stalled\n", (unsigned)(first & 0xFF));
            return -1;
        }
        if ((done & ~0x1FULL) == command_offset) return 0;
        cpu_relax();
    }
    kprintf("ITS: command %x was never read\n", (unsigned)(first & 0xFF));
    return -1;
}

static int allocate_tables(void) {
    for (unsigned index = 0; index < 8U; index++) {
        uint64_t value = read64(its, GITS_BASER + index * 8U);
        unsigned type = (unsigned)((value >> 56) & 7U);
        if (type != TABLE_TYPE_DEVICE && type != TABLE_TYPE_COLLECTION) continue;
        uint64_t entry = ((value >> 48) & 0x1FULL) + 1ULL;
        uint64_t entries = type == TABLE_TYPE_DEVICE ? (1ULL << device_bits) : 256ULL;
        uint64_t pages = (entries * entry + 4095ULL) / 4096ULL;
        if (pages > 256ULL) pages = 256ULL;
        uint64_t physical;
        if (!dma_alloc(pages * 4096ULL, 4096ULL, &physical)) return -1;
        write64(its, GITS_BASER + index * 8U,
                TABLE_VALID | ((uint64_t)type << 56) | ((entry - 1ULL) << 48) |
                physical | (pages - 1ULL));
        if ((read64(its, GITS_BASER + index * 8U) >> 8) & 3ULL) {
            kprintf("ITS: table %u refuses 4 KiB pages\n", index);
            return -1;
        }
    }
    return 0;
}

void its_init(void) {
    if (!aarch64_platform.gic_its) return;
    its = vmm_map_device(aarch64_platform.gic_its, GITS_BYTES);
    if (!its) return;
    write32(its, GITS_CTLR, 0);

    uint64_t typer = read64(its, GITS_TYPER);
    if (!(typer & 1ULL)) return;
    itt_entry_bytes = (unsigned)((typer >> 4) & 0xFULL) + 1U;
    device_bits = (unsigned)((typer >> 13) & 0x1FULL) + 1U;
    if (device_bits > MAX_DEVICE_BITS) device_bits = MAX_DEVICE_BITS;
    if (allocate_tables() != 0) return;

    uint64_t queue_physical;
    command_queue = (uint64_t)dma_alloc(COMMAND_QUEUE_BYTES, 4096ULL, &queue_physical);
    if (!command_queue) return;
    write64(its, GITS_CBASER, TABLE_VALID | queue_physical);
    write64(its, GITS_CWRITER, 0);
    command_offset = 0;

    uint64_t redistributor = gic_boot_redistributor();
    if (!redistributor || (read32(redistributor, GICR_CTLR) & 1U)) return;
    uint64_t properties_physical;
    lpi_properties = dma_alloc(LPI_COUNT, 4096ULL, &properties_physical);
    uint64_t pending_physical;
    if (!lpi_properties || !dma_alloc(0x10000ULL, 0x10000ULL, &pending_physical)) return;
    write64(redistributor, GICR_PROPBASER, properties_physical | (LPI_ID_BITS - 1U));
    write64(redistributor, GICR_PENDBASER, pending_physical | PENDING_ZEROED);
    write32(redistributor, GICR_CTLR, read32(redistributor, GICR_CTLR) | 1U);

    write32(its, GITS_CTLR, 1U);
    uint64_t target = (typer & (1ULL << 19))
        ? (gic_boot_redistributor_physical() & 0x000FFFFFFFFF0000ULL)
        : (((read64(redistributor, GICR_TYPER) >> 8) & 0xFFFFULL) << 16);
    if (send_command(COMMAND_MAPC, 0, TABLE_VALID | target) != 0) return;
    ready = 1;
    kprintf("ITS: message interrupts through %p, %u-bit device IDs\n",
            (void *)aarch64_platform.gic_its, device_bits);
}

int its_ready(void) {
    return ready;
}

int its_bind_msi(uint32_t device_id, uint32_t event, uint64_t *address) {
    if (!ready || event >= (1U << EVENT_BITS) || device_id >= (1U << device_bits)) return -1;
    unsigned known = 0;
    while (known < device_count && devices[known] != device_id) known++;
    if (known == device_count) {
        if (device_count >= MAX_DEVICES) return -1;
        uint64_t table_physical;
        if (!dma_alloc((uint64_t)itt_entry_bytes << EVENT_BITS, 4096ULL, &table_physical))
            return -1;
        if (send_command(COMMAND_MAPD | ((uint64_t)device_id << 32), EVENT_BITS - 1U,
                         TABLE_VALID | table_physical) != 0)
            return -1;
        devices[device_count++] = device_id;
    }
    lpi_properties[event] = LPI_ENABLED_PRIORITY;
    if (send_command(COMMAND_MAPTI | ((uint64_t)device_id << 32),
                     event | ((uint64_t)(LPI_FIRST + event) << 32), 0) != 0)
        return -1;
    if (send_command(COMMAND_INVALL, 0, 0) != 0) return -1;
    *address = aarch64_platform.gic_its + GITS_TRANSLATER;
    return 0;
}
