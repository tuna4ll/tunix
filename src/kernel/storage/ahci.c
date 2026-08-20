#include <stddef.h>
#include <stdint.h>
#include "../include/ahci.h"
#include "../include/block.h"
#include "../include/kstring.h"
#include "../include/pci.h"
#include "../include/pmm.h"
#include "../include/vmm.h"

/*
 * AHCI, enough of it to be a disk.
 *
 * The controller is a set of memory-mapped ports, each with a 32-slot command
 * list. A command is a header pointing at a command table; the table holds the
 * FIS the drive actually reads and a scatter/gather list of the memory to move.
 * Only slot 0 is ever used here -- commands are issued one at a time and waited
 * for, because nothing above this needs a queue yet and one outstanding command
 * removes every ordering question.
 *
 * The scatter/gather list is the part worth reading. A caller's buffer is a
 * kernel *virtual* range, and the kernel heap is not physically contiguous, so
 * the list is built one page at a time by translating each page separately.
 * Assuming contiguity is the bug this design exists to avoid: it works for
 * every small read and corrupts memory on the first large one.
 */

extern void kprintf(const char *fmt, ...);

#define AHCI_CLASS 0x01U
#define AHCI_SUBCLASS 0x06U
#define AHCI_PROG_IF 0x01U

/* Host registers. */
#define HBA_CAP 0x00U
#define HBA_GHC 0x04U
#define HBA_PI 0x0CU
#define HBA_GHC_AE 0x80000000U
#define HBA_GHC_HR 0x00000001U
#define HBA_CAP_S64A 0x80000000U

/* Port registers, at 0x100 + port * 0x80. */
#define PORT_BASE 0x100U
#define PORT_STRIDE 0x80U
#define PORT_CLB 0x00U
#define PORT_CLBU 0x04U
#define PORT_FB 0x08U
#define PORT_FBU 0x0CU
#define PORT_IS 0x10U
#define PORT_IE 0x14U
#define PORT_CMD 0x18U
#define PORT_TFD 0x20U
#define PORT_SIG 0x24U
#define PORT_SSTS 0x28U
#define PORT_SERR 0x30U
#define PORT_CI 0x38U

#define PORT_CMD_ST 0x0001U
#define PORT_CMD_FRE 0x0010U
#define PORT_CMD_FR 0x4000U
#define PORT_CMD_CR 0x8000U

#define TFD_BSY 0x80U
#define TFD_DRQ 0x08U
#define TFD_ERR 0x01U

#define SIG_SATA 0x00000101U

#define FIS_TYPE_H2D 0x27U
#define FIS_H2D_COMMAND 0x80U

#define ATA_READ_DMA_EXT 0x25U
#define ATA_WRITE_DMA_EXT 0x35U
#define ATA_FLUSH_CACHE_EXT 0xEAU
#define ATA_IDENTIFY 0xECU

/* 32 entries of 4 KiB is 128 KiB in one command, which is more than any caller
   asks for and still leaves the whole port fitting in a single page. */
#define AHCI_PRDT_ENTRIES 32U
#define AHCI_MAX_SECTORS (AHCI_PRDT_ENTRIES * 4096U / BLOCK_SECTOR_SIZE)
#define AHCI_MAX_PORTS 8U
#define AHCI_WAIT_SPINS 40000000U

/* Offsets inside the one page each port gets. The alignment each structure
   needs is what decides them: 1 KiB for the command list, 256 bytes for the
   received-FIS area, 128 bytes for the command table. */
#define PORT_PAGE_CL 0x000U
#define PORT_PAGE_FIS 0x400U
#define PORT_PAGE_CT 0x500U

struct ahci_command_header {
    uint16_t flags;
    uint16_t prdt_length;
    volatile uint32_t transferred;
    uint32_t table_low;
    uint32_t table_high;
    uint32_t reserved[4];
} __attribute__((packed));

struct ahci_prdt_entry {
    uint32_t address_low;
    uint32_t address_high;
    uint32_t reserved;
    uint32_t count;      /* byte count - 1, bit 31 asks for an interrupt */
} __attribute__((packed));

struct ahci_command_table {
    uint8_t command_fis[64];
    uint8_t atapi[16];
    uint8_t reserved[48];
    struct ahci_prdt_entry prdt[AHCI_PRDT_ENTRIES];
} __attribute__((packed));

struct ahci_fis_h2d {
    uint8_t type;
    uint8_t flags;
    uint8_t command;
    uint8_t feature_low;
    uint8_t lba0, lba1, lba2, device;
    uint8_t lba3, lba4, lba5, feature_high;
    uint8_t count_low, count_high, icc, control;
    uint8_t reserved[4];
} __attribute__((packed));

struct ahci_port {
    uint64_t registers;          /* virtual address of this port's block */
    uint64_t page_physical;
    uint8_t *page;
    uint64_t sectors;
    char name[16];
};

static uint64_t hba_base;
static struct ahci_port ports[AHCI_MAX_PORTS];
static unsigned port_count;

static uint32_t read32(uint64_t address) {
    return *(volatile uint32_t *)address;
}

static void write32(uint64_t address, uint32_t value) {
    *(volatile uint32_t *)address = value;
}

static void pause_cpu(void) { __asm__ volatile("pause"); }

/* --- port start/stop ----------------------------------------------------- */

static int stop_port(struct ahci_port *port) {
    uint32_t command = read32(port->registers + PORT_CMD);
    write32(port->registers + PORT_CMD, command & ~(PORT_CMD_ST | PORT_CMD_FRE));
    for (uint32_t spin = 0; spin < AHCI_WAIT_SPINS; spin++) {
        if (!(read32(port->registers + PORT_CMD) & (PORT_CMD_CR | PORT_CMD_FR)))
            return 0;
        pause_cpu();
    }
    return -1;
}

static void start_port(struct ahci_port *port) {
    for (uint32_t spin = 0; spin < AHCI_WAIT_SPINS; spin++) {
        if (!(read32(port->registers + PORT_CMD) & PORT_CMD_CR)) break;
        pause_cpu();
    }
    uint32_t command = read32(port->registers + PORT_CMD);
    write32(port->registers + PORT_CMD, command | PORT_CMD_FRE | PORT_CMD_ST);
}

/* --- issuing one command ------------------------------------------------- */

/*
 * Fill the scatter/gather list from a kernel virtual buffer, one page at a
 * time. Returns the number of entries used, or -1 if the buffer needs more
 * than the table holds or a page has no translation.
 */
static int build_prdt(struct ahci_command_table *table, const void *buffer,
                      uint32_t bytes) {
    uint64_t address = (uint64_t)(uintptr_t)buffer;
    unsigned used = 0;
    while (bytes) {
        if (used == AHCI_PRDT_ENTRIES) return -1;
        uint64_t physical = 0;
        if (vmm_translate(vmm_kernel_cr3(), address, &physical, NULL) != 0) return -1;
        uint32_t chunk = 4096U - (uint32_t)(address & 0xFFFULL);
        if (chunk > bytes) chunk = bytes;
        table->prdt[used].address_low = (uint32_t)physical;
        table->prdt[used].address_high = (uint32_t)(physical >> 32);
        table->prdt[used].reserved = 0;
        table->prdt[used].count = chunk - 1U;
        used++;
        address += chunk;
        bytes -= chunk;
    }
    return (int)used;
}

static int wait_for_completion(struct ahci_port *port) {
    for (uint32_t spin = 0; spin < AHCI_WAIT_SPINS; spin++) {
        if (!(read32(port->registers + PORT_CI) & 1U)) break;
        if (read32(port->registers + PORT_IS) & 0x40000000U) return -1; /* TFES */
        pause_cpu();
    }
    if (read32(port->registers + PORT_CI) & 1U) return -1;
    uint32_t status = read32(port->registers + PORT_TFD);
    if (status & (TFD_ERR | TFD_BSY | TFD_DRQ)) return -1;
    return 0;
}

/*
 * One command, start to finish. `write` decides the direction; `buffer` may be
 * NULL for a command that moves nothing (a cache flush).
 */
static int issue(struct ahci_port *port, uint8_t command, uint64_t lba,
                 uint32_t sectors, void *buffer, uint32_t bytes, int write) {
    struct ahci_command_header *header =
        (struct ahci_command_header *)(port->page + PORT_PAGE_CL);
    struct ahci_command_table *table =
        (struct ahci_command_table *)(port->page + PORT_PAGE_CT);

    memset(header, 0, sizeof(*header));
    memset(table, 0, sizeof(*table));

    int entries = 0;
    if (bytes) {
        entries = build_prdt(table, buffer, bytes);
        if (entries < 0) return -1;
    }

    struct ahci_fis_h2d *fis = (struct ahci_fis_h2d *)table->command_fis;
    fis->type = FIS_TYPE_H2D;
    fis->flags = FIS_H2D_COMMAND;
    fis->command = command;
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->device = 0x40U;            /* LBA mode; IDENTIFY ignores it */
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    fis->count_low = (uint8_t)sectors;
    fis->count_high = (uint8_t)(sectors >> 8);

    /* Command FIS length is counted in dwords, and the write bit is what tells
       the controller which way the scatter/gather list moves. */
    header->flags = (uint16_t)((sizeof(*fis) / 4U) | (write ? 0x40U : 0U));
    header->prdt_length = (uint16_t)entries;
    header->transferred = 0;
    uint64_t table_physical = port->page_physical + PORT_PAGE_CT;
    header->table_low = (uint32_t)table_physical;
    header->table_high = (uint32_t)(table_physical >> 32);

    write32(port->registers + PORT_IS, 0xFFFFFFFFU);
    write32(port->registers + PORT_SERR, 0xFFFFFFFFU);

    for (uint32_t spin = 0; spin < AHCI_WAIT_SPINS; spin++) {
        if (!(read32(port->registers + PORT_TFD) & (TFD_BSY | TFD_DRQ))) break;
        pause_cpu();
    }
    write32(port->registers + PORT_CI, 1U);
    return wait_for_completion(port);
}

/* --- the block layer's view ---------------------------------------------- */

static int ahci_read(void *context, uint64_t lba, uint32_t count, void *destination) {
    struct ahci_port *port = (struct ahci_port *)context;
    uint8_t *out = (uint8_t *)destination;
    while (count) {
        uint32_t chunk = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;
        if (issue(port, ATA_READ_DMA_EXT, lba, chunk, out,
                  chunk * BLOCK_SECTOR_SIZE, 0) != 0) return -1;
        out += chunk * BLOCK_SECTOR_SIZE;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int ahci_write(void *context, uint64_t lba, uint32_t count, const void *source) {
    struct ahci_port *port = (struct ahci_port *)context;
    const uint8_t *in = (const uint8_t *)source;
    while (count) {
        uint32_t chunk = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;
        if (issue(port, ATA_WRITE_DMA_EXT, lba, chunk, (void *)(uintptr_t)in,
                  chunk * BLOCK_SECTOR_SIZE, 1) != 0) return -1;
        in += chunk * BLOCK_SECTOR_SIZE;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int ahci_flush(void *context) {
    return issue((struct ahci_port *)context, ATA_FLUSH_CACHE_EXT, 0, 0, NULL, 0, 0);
}

/* --- bring-up ------------------------------------------------------------ */

static uint64_t identify_sectors(struct ahci_port *port) {
    uint8_t *buffer = (uint8_t *)vmm_phys_to_virt(port->page_physical);
    /* The tail of the port's own page is free and physically contiguous, which
       is exactly what a 512-byte IDENTIFY answer needs. */
    uint8_t *identify = buffer + 0x800U;
    memset(identify, 0, 512);
    if (issue(port, ATA_IDENTIFY, 0, 0, identify, 512, 0) != 0) return 0;

    uint16_t words[256];
    memcpy(words, identify, sizeof(words));
    /* Word 83 bit 10 says the drive speaks 48-bit addressing, and then words
       100..103 hold the count; otherwise it is the 32-bit one in 60..61. */
    if (words[83] & (1U << 10)) {
        uint64_t sectors = 0;
        for (int index = 3; index >= 0; index--)
            sectors = (sectors << 16) | words[100 + index];
        if (sectors) return sectors;
    }
    return ((uint64_t)words[61] << 16) | words[60];
}

static void bring_up_port(unsigned index) {
    if (port_count == AHCI_MAX_PORTS) return;
    uint64_t registers = hba_base + PORT_BASE + (uint64_t)index * PORT_STRIDE;

    uint32_t status = read32(registers + PORT_SSTS);
    if ((status & 0x0FU) != 3U || ((status >> 8) & 0x0FU) != 1U) return;
    if (read32(registers + PORT_SIG) != SIG_SATA) return;

    struct ahci_port *port = &ports[port_count];
    memset(port, 0, sizeof(*port));
    port->registers = registers;

    if (stop_port(port) != 0) return;

    uint64_t physical = (uint64_t)pmm_alloc_page();
    if (!physical) return;
    port->page_physical = physical;
    port->page = (uint8_t *)vmm_phys_to_virt(physical);
    memset(port->page, 0, 4096);

    uint64_t list_physical = physical + PORT_PAGE_CL;
    uint64_t fis_physical = physical + PORT_PAGE_FIS;
    write32(registers + PORT_CLB, (uint32_t)list_physical);
    write32(registers + PORT_CLBU, (uint32_t)(list_physical >> 32));
    write32(registers + PORT_FB, (uint32_t)fis_physical);
    write32(registers + PORT_FBU, (uint32_t)(fis_physical >> 32));
    write32(registers + PORT_SERR, 0xFFFFFFFFU);
    write32(registers + PORT_IE, 0);

    start_port(port);

    port->sectors = identify_sectors(port);
    if (!port->sectors) {
        stop_port(port);
        pmm_free_page((void *)physical);
        return;
    }

    struct block_device device;
    memset(&device, 0, sizeof(device));
    device.name[0] = 'a'; device.name[1] = 'h'; device.name[2] = 'c';
    device.name[3] = 'i'; device.name[4] = (char)('0' + port_count);
    device.sectors = port->sectors;
    device.read = ahci_read;
    device.write = ahci_write;
    device.flush = ahci_flush;
    device.context = port;
    if (block_register(&device) < 0) return;
    port_count++;
}

void ahci_init(void) {
    struct pci_device pci;
    if (pci_find_class(AHCI_CLASS, AHCI_SUBCLASS, &pci) != 0) return;
    if (pci.prog_if != AHCI_PROG_IF) return;

    /* ABAR is BAR5 and is always 32-bit memory space. */
    uint64_t abar = pci.bar[5] & ~0xFULL;
    if (!abar) return;

    pci_enable_bus_mastering(&pci);
    hba_base = vmm_map_device(abar, 0x2000U);
    if (!hba_base) {
        kprintf("AHCI: register window unavailable\n");
        return;
    }

    write32(hba_base + HBA_GHC, read32(hba_base + HBA_GHC) | HBA_GHC_AE);

    uint32_t implemented = read32(hba_base + HBA_PI);
    for (unsigned index = 0; index < 32U; index++) {
        if (!(implemented & (1U << index))) continue;
        bring_up_port(index);
    }
    if (!port_count) kprintf("AHCI: controller present, no disks\n");
}
