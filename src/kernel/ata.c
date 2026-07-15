#include <stddef.h>
#include <stdint.h>
#include "include/ata.h"
#include "include/io.h"
#include "include/kstring.h"
#include "include/pci.h"

#define ATA_DATA       0x1F0
#define ATA_SECCOUNT0  0x1F2
#define ATA_LBA0       0x1F3
#define ATA_LBA1       0x1F4
#define ATA_LBA2       0x1F5
#define ATA_HDDEVSEL   0x1F6
#define ATA_COMMAND    0x1F7
#define ATA_STATUS     0x1F7
#define ATA_CONTROL    0x3F6

#define ATA_CMD_READ_PIO 0x20
#define ATA_CMD_READ_DMA 0xC8
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_WRITE_DMA 0xCA
#define ATA_CMD_FLUSH_CACHE 0xE7
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_SR_BSY       0x80
#define ATA_SR_DRQ       0x08
#define ATA_SR_DF        0x20
#define ATA_SR_ERR       0x01
#define ATA_SECTOR_SIZE  512U
#define ATA_DMA_MAX_SECTORS 256U
#define ATA_DMA_MAX_PRDS 4U
#define KERNEL_VIRTUAL_BASE 0xFFFFFFFF80000000ULL

#define BM_COMMAND_START 0x01
#define BM_COMMAND_READ  0x08
#define BM_STATUS_ACTIVE 0x01
#define BM_STATUS_ERROR  0x02
#define BM_STATUS_IRQ    0x04

struct ata_prd {
    uint32_t physical_address;
    uint16_t byte_count;
    uint16_t flags;
} __attribute__((packed));

#define ATA_PRD_END 0x8000U

static uint32_t cached_sectors;
static int identify_attempted;
static int dma_probe_state;
static uint16_t dma_io_base;
static struct ata_prd dma_prdt[ATA_DMA_MAX_PRDS] __attribute__((aligned(16)));

static inline uint64_t ata_pointer_physical(const void *pointer) {
    uint64_t value = (uint64_t)(uintptr_t)pointer;
    if (value >= KERNEL_VIRTUAL_BASE) value -= KERNEL_VIRTUAL_BASE;
    return value;
}

/*
 * DMA needs physically contiguous memory. Raw physical addresses and the
 * kernel image mapping (KERNEL_VIRTUAL_BASE + first GiB) qualify; kernel
 * heap mappings above that are pieced together from arbitrary pages, so
 * their virtual-minus-base "physical" address would be garbage.
 */
static int ata_dma_pointer_valid(const void *pointer) {
    uint64_t value = (uint64_t)(uintptr_t)pointer;
    if (value < KERNEL_VIRTUAL_BASE) return 1;
    return value - KERNEL_VIRTUAL_BASE < 0x40000000ULL;
}

static inline void ata_read_words(uint16_t *destination, size_t word_count) {
    __asm__ volatile("cld; rep insw"
                     : "+D"(destination), "+c"(word_count)
                     : "d"((uint16_t)ATA_DATA)
                     : "memory");
}

static inline void ata_write_words(const uint16_t *source, size_t word_count) {
    __asm__ volatile("cld; rep outsw"
                     : "+S"(source), "+c"(word_count)
                     : "d"((uint16_t)ATA_DATA)
                     : "memory");
}

static int ata_wait_not_busy(void) {
    for (uint32_t timeout = 0; timeout < 10000000U; timeout++) {
        uint8_t status = inb(ATA_STATUS);
        if (!(status & ATA_SR_BSY)) return status;
        __asm__ volatile("pause");
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (uint32_t timeout = 0; timeout < 10000000U; timeout++) {
        uint8_t status = inb(ATA_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

static void ata_soft_reset(void) {
    outb(ATA_CONTROL, 0x04U);
    for (unsigned index = 0; index < 4U; index++) io_wait();
    outb(ATA_CONTROL, 0x00U);
    for (unsigned index = 0; index < 4U; index++) io_wait();
    (void)ata_wait_not_busy();
}

static int ata_dma_probe(void) {
    if (dma_probe_state) return dma_probe_state > 0 ? 0 : -1;
    dma_probe_state = -1;

    struct pci_device controller;
    if (pci_find_class(0x01U, 0x01U, &controller) != 0) return -1;
    if (!(controller.prog_if & 0x80U)) return -1;

    uint32_t bar4 = controller.bar[4];
    if (!(bar4 & 0x01U)) return -1;
    uint32_t base = bar4 & ~0x0FU;
    if (!base || base > 0xFFF0U) return -1;

    pci_enable_bus_mastering(&controller);
    dma_io_base = (uint16_t)base;
    dma_probe_state = 1;
    return 0;
}

static int ata_build_prdt(uint64_t destination, uint32_t byte_count) {
    unsigned entries = 0;
    while (byte_count) {
        if (entries >= ATA_DMA_MAX_PRDS || destination > 0xFFFFFFFFULL) return -1;

        uint32_t boundary = 0x10000U - (uint32_t)(destination & 0xFFFFU);
        uint32_t chunk = byte_count;
        if (chunk > boundary) chunk = boundary;
        if (chunk > 0x10000U) chunk = 0x10000U;

        dma_prdt[entries].physical_address = (uint32_t)destination;
        dma_prdt[entries].byte_count = chunk == 0x10000U ? 0U : (uint16_t)chunk;
        dma_prdt[entries].flags = 0;

        destination += chunk;
        byte_count -= chunk;
        entries++;
    }

    if (!entries) return -1;
    dma_prdt[entries - 1U].flags = ATA_PRD_END;
    return 0;
}

static int ata_dma_transfer_chunk(uint32_t lba, uint32_t sectors,
                                  uint64_t buffer_physical, int to_device) {
    uint32_t bytes = sectors * ATA_SECTOR_SIZE;
    if (ata_build_prdt(buffer_physical, bytes) != 0) return -1;

    uint64_t prdt_physical = ata_pointer_physical(dma_prdt);
    if (prdt_physical > 0xFFFFFFFFULL) return -1;

    uint8_t direction = to_device ? 0U : BM_COMMAND_READ;
    uint8_t ata_command = to_device ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA;
    uint16_t command_port = dma_io_base;
    uint16_t status_port = (uint16_t)(dma_io_base + 2U);
    uint16_t prdt_port = (uint16_t)(dma_io_base + 4U);

    outb(command_port, 0);
    outb(status_port, BM_STATUS_ERROR | BM_STATUS_IRQ);
    outl(prdt_port, (uint32_t)prdt_physical);

    if (ata_wait_not_busy() < 0) return -1;
    outb(ATA_HDDEVSEL, (uint8_t)(0xE0U | ((lba >> 24) & 0x0FU)));
    io_wait();
    outb(ATA_SECCOUNT0, sectors == 256U ? 0U : (uint8_t)sectors);
    outb(ATA_LBA0, (uint8_t)lba);
    outb(ATA_LBA1, (uint8_t)(lba >> 8));
    outb(ATA_LBA2, (uint8_t)(lba >> 16));

    __asm__ volatile("" : : : "memory");
    outb(command_port, direction);
    outb(ATA_COMMAND, ata_command);
    outb(command_port, (uint8_t)(direction | BM_COMMAND_START));

    int result = -1;
    for (uint32_t timeout = 0; timeout < 100000000U; timeout++) {
        uint8_t bus_status = inb(status_port);
        uint8_t ata_status = inb(ATA_STATUS);
        if ((bus_status & BM_STATUS_ERROR) || (ata_status & (ATA_SR_ERR | ATA_SR_DF))) {
            break;
        }
        if (!(bus_status & BM_STATUS_ACTIVE) && !(ata_status & ATA_SR_BSY)) {
            result = 0;
            break;
        }
        __asm__ volatile("pause");
    }

    outb(command_port, direction);
    uint8_t final_bus_status = inb(status_port);
    outb(status_port, BM_STATUS_ERROR | BM_STATUS_IRQ);
    __asm__ volatile("" : : : "memory");

    if (result != 0 || (final_bus_status & BM_STATUS_ERROR)) {
        ata_soft_reset();
        return -1;
    }
    return 0;
}

uint32_t ata_disk_sectors(void) {
    if (identify_attempted) return cached_sectors;
    identify_attempted = 1;

    outb(ATA_HDDEVSEL, 0xA0U);
    io_wait();
    outb(ATA_SECCOUNT0, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(ATA_STATUS);
    if (status == 0) return 0;
    if (ata_wait_not_busy() < 0) return 0;
    if (inb(ATA_LBA1) != 0 || inb(ATA_LBA2) != 0) return 0;
    if (ata_wait_drq() != 0) return 0;

    uint16_t identify[256];
    ata_read_words(identify, 256U);
    cached_sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
    if (cached_sectors > 0x10000000U) cached_sectors = 0x10000000U;
    return cached_sectors;
}

static int ata_dma_transfer28(uint32_t lba, uint32_t sectors,
                              const void *buffer, int to_device) {
    if (!buffer || sectors == 0 || lba > 0x0FFFFFFFU ||
        sectors > 0x10000000U - lba) return -1;
    if (!ata_dma_pointer_valid(buffer)) return -1;
    if (ata_dma_probe() != 0) return -1;

    uint32_t disk_sectors = ata_disk_sectors();
    if (disk_sectors && (lba >= disk_sectors || sectors > disk_sectors - lba)) return -1;

    uint64_t physical = ata_pointer_physical(buffer);
    uint64_t total_bytes = (uint64_t)sectors * ATA_SECTOR_SIZE;
    if (physical > 0xFFFFFFFFULL || total_bytes > 0x100000000ULL - physical) return -1;

    uint32_t remaining = sectors;
    uint32_t current_lba = lba;
    while (remaining) {
        uint32_t batch = remaining > ATA_DMA_MAX_SECTORS ? ATA_DMA_MAX_SECTORS : remaining;
        if (ata_dma_transfer_chunk(current_lba, batch, physical, to_device) != 0) return -1;
        current_lba += batch;
        remaining -= batch;
        physical += (uint64_t)batch * ATA_SECTOR_SIZE;
    }
    return 0;
}

int ata_dma_read28(uint32_t lba, uint32_t sectors, void *destination) {
    return ata_dma_transfer28(lba, sectors, destination, 0);
}

int ata_dma_write28(uint32_t lba, uint32_t sectors, const void *source) {
    return ata_dma_transfer28(lba, sectors, source, 1);
}

int ata_pio_read28(uint32_t lba, uint32_t sectors, void *destination) {
    if (!destination || sectors == 0 || lba > 0x0FFFFFFFU ||
        sectors > 0x10000000U - lba) return -1;

    uint32_t disk_sectors = ata_disk_sectors();
    if (disk_sectors && (lba >= disk_sectors || sectors > disk_sectors - lba)) return -1;

    uint16_t *output = (uint16_t *)destination;
    uint32_t remaining = sectors;
    uint32_t current_lba = lba;

    while (remaining) {
        uint8_t batch = (uint8_t)(remaining > 255U ? 255U : remaining);

        outb(ATA_HDDEVSEL, (uint8_t)(0xE0U | ((current_lba >> 24) & 0x0FU)));
        io_wait();
        outb(ATA_SECCOUNT0, batch);
        outb(ATA_LBA0, (uint8_t)current_lba);
        outb(ATA_LBA1, (uint8_t)(current_lba >> 8));
        outb(ATA_LBA2, (uint8_t)(current_lba >> 16));
        outb(ATA_COMMAND, ATA_CMD_READ_PIO);

        for (uint32_t sector = 0; sector < batch; sector++) {
            if (ata_wait_drq() != 0) return -1;
            ata_read_words(output, 256U);
            output += 256U;
        }

        current_lba += batch;
        remaining -= batch;
    }
    return 0;
}

int ata_flush_cache(void) {
    if (ata_wait_not_busy() < 0) return -1;
    outb(ATA_HDDEVSEL, 0xE0U);
    io_wait();
    outb(ATA_COMMAND, ATA_CMD_FLUSH_CACHE);
    int status = ata_wait_not_busy();
    if (status < 0 || (status & (ATA_SR_ERR | ATA_SR_DF))) return -1;
    return 0;
}

int ata_pio_write28(uint32_t lba, uint32_t sectors, const void *source) {
    if (!source || sectors == 0 || lba > 0x0FFFFFFFU ||
        sectors > 0x10000000U - lba) return -1;

    uint32_t disk_sectors = ata_disk_sectors();
    if (disk_sectors && (lba >= disk_sectors || sectors > disk_sectors - lba)) return -1;

    const uint16_t *input = (const uint16_t *)source;
    uint32_t remaining = sectors;
    uint32_t current_lba = lba;

    while (remaining) {
        uint8_t batch = (uint8_t)(remaining > 255U ? 255U : remaining);

        if (ata_wait_not_busy() < 0) return -1;
        outb(ATA_HDDEVSEL, (uint8_t)(0xE0U | ((current_lba >> 24) & 0x0FU)));
        io_wait();
        outb(ATA_SECCOUNT0, batch);
        outb(ATA_LBA0, (uint8_t)current_lba);
        outb(ATA_LBA1, (uint8_t)(current_lba >> 8));
        outb(ATA_LBA2, (uint8_t)(current_lba >> 16));
        outb(ATA_COMMAND, ATA_CMD_WRITE_PIO);

        for (uint32_t sector = 0; sector < batch; sector++) {
            if (ata_wait_drq() != 0) return -1;
            ata_write_words(input, 256U);
            input += 256U;
        }

        int status = ata_wait_not_busy();
        if (status < 0 || (status & (ATA_SR_ERR | ATA_SR_DF))) return -1;
        current_lba += batch;
        remaining -= batch;
    }
    return 0;
}

int ata_pio_read_bytes(uint64_t offset, size_t size, void *destination) {
    if (!destination) return -1;
    if (!size) return 0;

    uint32_t sectors = ata_disk_sectors();
    uint64_t disk_size = (uint64_t)sectors * ATA_SECTOR_SIZE;
    if (!sectors || offset >= disk_size) return 0;
    if ((uint64_t)size > disk_size - offset) size = (size_t)(disk_size - offset);

    uint8_t bounce[ATA_SECTOR_SIZE];
    uint8_t *out = (uint8_t *)destination;
    size_t completed = 0;
    while (completed < size) {
        uint64_t absolute = offset + completed;
        uint32_t lba = (uint32_t)(absolute / ATA_SECTOR_SIZE);
        size_t in_sector = (size_t)(absolute % ATA_SECTOR_SIZE);
        size_t chunk = ATA_SECTOR_SIZE - in_sector;
        if (chunk > size - completed) chunk = size - completed;
        if (ata_pio_read28(lba, 1, bounce) != 0) return completed ? (int)completed : -1;
        memcpy(out + completed, bounce + in_sector, chunk);
        completed += chunk;
    }
    return (int)completed;
}
