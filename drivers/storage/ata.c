#include <stddef.h>
#include <stdint.h>
#include <tunix/ata.h>
#include <tunix/block.h>
#include <tunix/cdrom.h>
#include <tunix/heap.h>
#include <tunix/kprintf.h>
#include <tunix/time.h>
#include <tunix/cpu.h>
#include <tunix/io.h>
#include <tunix/kstring.h>
#include <tunix/pci.h>
#include <tunix/vmm.h>
#include <tunix/iowait.h>
#include <tunix/lock.h>
#include <tunix/mutex.h>

#define ATA_DATA      0x1F0
#define ATA_SECCOUNT0 0x1F2
#define ATA_LBA0      0x1F3
#define ATA_LBA1      0x1F4
#define ATA_LBA2      0x1F5
#define ATA_HDDEVSEL  0x1F6
#define ATA_COMMAND   0x1F7
#define ATA_STATUS    0x1F7
#define ATA_CONTROL   0x3F6

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_READ_DMA    0xC8
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_WRITE_DMA   0xCA
#define ATA_CMD_FLUSH_CACHE 0xE7
#define ATA_CMD_IDENTIFY    0xEC
#define ATA_SR_BSY          0x80
#define ATA_SR_DRQ          0x08
#define ATA_SR_DF           0x20
#define ATA_SR_ERR          0x01
#define ATA_SECTOR_SIZE     512U
#define ATA_DMA_MAX_SECTORS 256U
#define ATA_DMA_MAX_PRDS    4U

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

static struct mutex ata_lock = MUTEX_INITIALIZER("ata", LOCK_RANK_BLOCK);

static inline uint64_t ata_pointer_physical(const void *pointer, uint64_t length) {
    return vmm_dma_physical(pointer, length);
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

static int ata_idle(void *unused) {
    (void)unused;
    return !(inb(ATA_STATUS) & ATA_SR_BSY);
}

static int ata_wait_not_busy(void) {
    if (io_poll(ata_idle, NULL, IO_TIMEOUT_NS) != 0) return -1;
    return inb(ATA_STATUS);
}

static int ata_data_or_error(void *unused) {
    (void)unused;
    uint8_t status = inb(ATA_STATUS);
    return (status & (ATA_SR_ERR | ATA_SR_DF)) || (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ));
}

static int ata_wait_drq(void) {
    if (io_poll(ata_data_or_error, NULL, IO_TIMEOUT_NS) != 0) return -1;
    uint8_t status = inb(ATA_STATUS);
    return (status & (ATA_SR_ERR | ATA_SR_DF)) ? -1 : 0;
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

static int ata_dma_settled(void *context) {
    uint16_t status_port = *(const uint16_t *)context;
    uint8_t bus_status = inb(status_port);
    uint8_t ata_status = inb(ATA_STATUS);
    if ((bus_status & BM_STATUS_ERROR) || (ata_status & (ATA_SR_ERR | ATA_SR_DF))) return 1;
    return !(bus_status & BM_STATUS_ACTIVE) && !(ata_status & ATA_SR_BSY);
}

static int ata_dma_transfer_chunk(uint32_t lba, uint32_t sectors, uint64_t buffer_physical,
                                  int to_device) {
    uint32_t bytes = sectors * ATA_SECTOR_SIZE;
    if (ata_build_prdt(buffer_physical, bytes) != 0) return -1;

    uint64_t prdt_physical = ata_pointer_physical(dma_prdt, sizeof dma_prdt);
    if (!prdt_physical || prdt_physical > 0xFFFFFFFFULL) return -1;

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
    if (io_poll(ata_dma_settled, &status_port, IO_TIMEOUT_NS) == 0) {
        uint8_t bus_status = inb(status_port);
        uint8_t ata_status = inb(ATA_STATUS);
        if (!(bus_status & BM_STATUS_ERROR) && !(ata_status & (ATA_SR_ERR | ATA_SR_DF))) result = 0;
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

static int ata_controller_present(void) {
    if (inb(ATA_STATUS) == 0xFFU) return 0;
    outb(ATA_LBA0, 0x55U);
    outb(ATA_LBA1, 0xAAU);
    if (inb(ATA_LBA0) != 0x55U || inb(ATA_LBA1) != 0xAAU) return 0;
    return 1;
}

uint32_t ata_disk_sectors(void) {
    if (identify_attempted) return cached_sectors;
    identify_attempted = 1;

    if (!ata_controller_present()) return 0;

    outb(ATA_HDDEVSEL, 0xA0U);
    io_wait();
    outb(ATA_SECCOUNT0, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(ATA_STATUS);
    if (status == 0 || status == 0xFFU) return 0;
    if (ata_wait_not_busy() < 0) return 0;
    if (inb(ATA_LBA1) != 0 || inb(ATA_LBA2) != 0) return 0;
    if (ata_wait_drq() != 0) return 0;

    uint16_t identify[256];
    ata_read_words(identify, 256U);
    cached_sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
    if (cached_sectors > 0x10000000U) cached_sectors = 0x10000000U;
    return cached_sectors;
}

static int ata_dma_transfer28(uint32_t lba, uint32_t sectors, const void *buffer, int to_device) {
    if (!buffer || sectors == 0 || lba > 0x0FFFFFFFU || sectors > 0x10000000U - lba) return -1;
    if (ata_dma_probe() != 0) return -1;

    uint32_t disk_sectors = ata_disk_sectors();
    if (disk_sectors && (lba >= disk_sectors || sectors > disk_sectors - lba)) return -1;

    uint64_t total_bytes = (uint64_t)sectors * ATA_SECTOR_SIZE;
    uint64_t physical = ata_pointer_physical(buffer, total_bytes);
    if (!physical || physical > 0xFFFFFFFFULL || total_bytes > 0x100000000ULL - physical) return -1;

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
    if (!destination || sectors == 0 || lba > 0x0FFFFFFFU || sectors > 0x10000000U - lba) return -1;

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

static int ata_flush_cache_unlocked(void) {
    if (ata_wait_not_busy() < 0) return -1;
    outb(ATA_HDDEVSEL, 0xE0U);
    io_wait();
    outb(ATA_COMMAND, ATA_CMD_FLUSH_CACHE);
    int status = ata_wait_not_busy();
    if (status < 0 || (status & (ATA_SR_ERR | ATA_SR_DF))) return -1;
    return 0;
}

int ata_flush_cache(void) {
    mutex_lock(&ata_lock);
    int status = ata_flush_cache_unlocked();
    mutex_unlock(&ata_lock);
    return status;
}

int ata_pio_write28(uint32_t lba, uint32_t sectors, const void *source) {
    if (!source || sectors == 0 || lba > 0x0FFFFFFFU || sectors > 0x10000000U - lba) return -1;

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

static int ata_block_read_unlocked(void *context, uint64_t lba, uint32_t count, void *destination) {
    (void)context;
    if (lba > 0x0FFFFFFFULL) return -1;
    if (ata_dma_read28((uint32_t)lba, count, destination) == 0) return 0;
    return ata_pio_read28((uint32_t)lba, count, destination);
}

static int ata_block_read(void *context, uint64_t lba, uint32_t count, void *destination) {
    mutex_lock(&ata_lock);
    int status = ata_block_read_unlocked(context, lba, count, destination);
    mutex_unlock(&ata_lock);
    return status;
}

static int ata_block_write_unlocked(void *context, uint64_t lba, uint32_t count,
                                    const void *source) {
    (void)context;
    if (lba > 0x0FFFFFFFULL) return -1;
    if (ata_dma_write28((uint32_t)lba, count, source) == 0) return 0;
    return ata_pio_write28((uint32_t)lba, count, source);
}

static int ata_block_write(void *context, uint64_t lba, uint32_t count, const void *source) {
    mutex_lock(&ata_lock);
    int status = ata_block_write_unlocked(context, lba, count, source);
    mutex_unlock(&ata_lock);
    return status;
}

static int ata_block_flush(void *context) {
    (void)context;
    return ata_flush_cache();
}

// Optical drives on either IDE channel, master or slave, spoken to by PIO
// PACKET commands (ATA/ATAPI-6, 8.23).

#define IDE_FEATURES      1U
#define IDE_BYTES_LOW     4U
#define IDE_BYTES_HIGH    5U
#define IDE_SELECT        6U
#define IDE_COMMAND       7U
#define IDE_CMD_PACKET    0xA0U
#define IDE_CMD_IDENTIFY_PACKET 0xA1U
#define IDE_CONTROL_SRST  0x04U
#define IDE_CONTROL_NIEN  0x02U
// The most a drive is asked to hand over per DRQ, an even number below 64 KiB.
#define IDE_PACKET_BYTES  0xF800U
#define IDE_PACKET_MAX    (32U * 2048U)

struct ide_channel {
    uint16_t base;
    uint16_t control;
    // The channel's bus master registers, for DMA; 0 when it has none.
    uint16_t bus_master;
};

struct ide_atapi {
    struct ide_channel channel;
    uint8_t select;
};

static void ide_delay(const struct ide_channel *channel) {
    // Reading the alternate status four times is the 400 ns a new status needs.
    for (int index = 0; index < 4; index++) (void)inb(channel->control);
}

static int ide_not_busy(void *context) {
    const struct ide_channel *channel = context;
    return !(inb(channel->base + IDE_COMMAND) & ATA_SR_BSY);
}

static int ide_wait(const struct ide_channel *channel) {
    if (io_poll(ide_not_busy, (void *)channel, IO_TIMEOUT_NS) != 0) return -1;
    return inb(channel->base + IDE_COMMAND);
}

static int ide_select(const struct ide_atapi *drive) {
    if (ide_wait(&drive->channel) < 0) return -1;
    outb(drive->channel.base + IDE_SELECT, drive->select);
    ide_delay(&drive->channel);
    return ide_wait(&drive->channel);
}

// After a refused command the error register's high nibble is the sense key.
static int ide_refused(const struct ide_channel *channel) {
    int key = inb(channel->base + IDE_FEATURES) >> 4;
    return key ? key : -1;
}

static void ide_read_data(const struct ide_channel *channel, uint8_t *out, uint32_t *done,
                          uint32_t wanted, uint32_t offered) {
    uint32_t words = (offered + 1U) / 2U;
    uint32_t room = (wanted - *done) / 2U;
    uint32_t take = words < room ? words : room;
    uint16_t port = channel->base;
    uint16_t *destination = (uint16_t *)(void *)(out + *done);
    size_t count = take;
    __asm__ volatile("cld; rep insw" : "+D"(destination), "+c"(count) : "d"(port) : "memory");
    *done += take * 2U;
    // More than was asked for is read and dropped, or the drive would wait for it.
    for (uint32_t extra = take; extra < words; extra++) (void)inw(port);
}

static int ide_packet_unlocked(struct ide_atapi *drive, const uint8_t *packet, void *buffer,
                               uint32_t bytes) {
    const struct ide_channel *channel = &drive->channel;
    if (ide_select(drive) < 0) return -1;
    outb(channel->base + IDE_FEATURES, 0);
    outb(channel->base + IDE_BYTES_LOW, (uint8_t)IDE_PACKET_BYTES);
    outb(channel->base + IDE_BYTES_HIGH, (uint8_t)(IDE_PACKET_BYTES >> 8));
    outb(channel->base + IDE_COMMAND, IDE_CMD_PACKET);
    ide_delay(channel);
    int status = ide_wait(channel);
    if (status < 0) return -1;
    if (status & (ATA_SR_ERR | ATA_SR_DF)) return ide_refused(channel);
    if (!(status & ATA_SR_DRQ)) return -1;
    const uint16_t *words = (const uint16_t *)(const void *)packet;
    for (int index = 0; index < 6; index++) outw(channel->base, words[index]);

    uint32_t done = 0;
    for (;;) {
        ide_delay(channel);
        status = ide_wait(channel);
        if (status < 0) return -1;
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return ide_refused(channel);
        if (!(status & ATA_SR_DRQ)) break;
        uint32_t offered = (uint32_t)inb(channel->base + IDE_BYTES_LOW) |
            (uint32_t)inb(channel->base + IDE_BYTES_HIGH) << 8;
        if (!buffer || !offered) return -1;
        ide_read_data(channel, (uint8_t *)buffer, &done, bytes, offered);
    }
    return done == bytes ? 0 : -1;
}

// A PRD a page: the buffer need not be contiguous, as the block layer's are not.
static struct ata_prd atapi_prdt[IDE_PACKET_MAX / 4096U + 1U] __attribute__((aligned(16)));

static int ide_build_prdt(const void *buffer, uint32_t bytes) {
    uint64_t address = (uint64_t)(uintptr_t)buffer;
    uint64_t cr3 = vmm_kernel_cr3();
    unsigned used = 0;
    if ((bytes & 1U) || (address & 1U)) return -1;
    while (bytes) {
        uint64_t physical = 0;
        if (used == sizeof atapi_prdt / sizeof atapi_prdt[0] || !cr3 ||
            vmm_translate(cr3, address, &physical, NULL) != 0 || !physical)
            return -1;
        uint32_t chunk = 4096U - (uint32_t)(address & 0xFFFULL);
        if (chunk > bytes) chunk = bytes;
        if (physical + chunk > 0x100000000ULL) return -1;
        atapi_prdt[used].physical_address = (uint32_t)physical;
        atapi_prdt[used].byte_count = (uint16_t)chunk;
        atapi_prdt[used].flags = 0;
        used++;
        address += chunk;
        bytes -= chunk;
    }
    if (!used) return -1;
    atapi_prdt[used - 1U].flags = ATA_PRD_END;
    return 0;
}

static int ide_bus_master_settled(void *context) {
    const struct ide_channel *channel = context;
    uint8_t bus = inb((uint16_t)(channel->bus_master + 2U));
    uint8_t status = inb(channel->base + IDE_COMMAND);
    if ((bus & BM_STATUS_ERROR) || (status & (ATA_SR_ERR | ATA_SR_DF))) return 1;
    return !(bus & BM_STATUS_ACTIVE) && !(status & ATA_SR_BSY);
}

// The data of a read by bus master DMA, which moves it many times faster than
// PIO: 0, the sense key, -1 when it failed, or 1 when DMA cannot be used here.
static int ide_packet_dma(struct ide_atapi *drive, const uint8_t *packet, void *buffer,
                          uint32_t bytes) {
    const struct ide_channel *channel = &drive->channel;
    uint64_t table = ata_pointer_physical(atapi_prdt, sizeof atapi_prdt);
    if (!channel->bus_master || !table || table > 0xFFFFFFFFULL ||
        ide_build_prdt(buffer, bytes) != 0)
        return 1;
    uint16_t command = channel->bus_master;
    uint16_t bus_status = (uint16_t)(channel->bus_master + 2U);
    if (ide_select(drive) < 0) return -1;
    outb(command, 0);
    outb(bus_status, BM_STATUS_ERROR | BM_STATUS_IRQ);
    outl((uint16_t)(channel->bus_master + 4U), (uint32_t)table);
    outb(command, BM_COMMAND_READ);
    outb(channel->base + IDE_FEATURES, 1);
    outb(channel->base + IDE_BYTES_LOW, 0);
    outb(channel->base + IDE_BYTES_HIGH, 0);
    outb(channel->base + IDE_COMMAND, IDE_CMD_PACKET);
    ide_delay(channel);
    int status = ide_wait(channel);
    if (status < 0) return -1;
    if (status & (ATA_SR_ERR | ATA_SR_DF)) return ide_refused(channel);
    if (!(status & ATA_SR_DRQ)) return -1;
    const uint16_t *words = (const uint16_t *)(const void *)packet;
    for (int index = 0; index < 6; index++) outw(channel->base, words[index]);
    __asm__ volatile("" : : : "memory");
    outb(command, BM_COMMAND_READ | BM_COMMAND_START);
    int result = io_poll(ide_bus_master_settled, (void *)channel, IO_TIMEOUT_NS) == 0 ? 0 : -1;
    outb(command, BM_COMMAND_READ);
    uint8_t bus = inb(bus_status);
    outb(bus_status, BM_STATUS_ERROR | BM_STATUS_IRQ);
    __asm__ volatile("" : : : "memory");
    status = ide_wait(channel);
    if (status < 0) return -1;
    if (status & (ATA_SR_ERR | ATA_SR_DF)) return ide_refused(channel);
    return result == 0 && !(bus & BM_STATUS_ERROR) ? 0 : -1;
}

static int ide_packet(void *context, const uint8_t *packet, void *buffer, uint32_t bytes) {
    struct ide_atapi *drive = (struct ide_atapi *)context;
    mutex_lock(&ata_lock);
    // Whole blocks by DMA, short replies (and what DMA cannot reach) by PIO.
    int status = bytes >= 2048U ? ide_packet_dma(drive, packet, buffer, bytes) : 1;
    if (status == 1) status = ide_packet_unlocked(drive, packet, buffer, bytes);
    mutex_unlock(&ata_lock);
    return status;
}

static void ide_reset(const struct ide_channel *channel) {
    outb(channel->control, IDE_CONTROL_SRST | IDE_CONTROL_NIEN);
    uint64_t until = time_uptime_ns() + 10000ULL;
    while (time_uptime_ns() < until) cpu_relax();
    outb(channel->control, IDE_CONTROL_NIEN);
    until = time_uptime_ns() + 2000000ULL;
    while (time_uptime_ns() < until) cpu_relax();
    (void)ide_wait(channel);
}

// IDENTIFY PACKET DEVICE: a packet device whose type is CD/DVD (5).
static int ide_is_optical(struct ide_atapi *drive) {
    const struct ide_channel *channel = &drive->channel;
    if (ide_select(drive) < 0) return 0;
    outb(channel->base + IDE_COMMAND, IDE_CMD_IDENTIFY_PACKET);
    ide_delay(channel);
    int status = ide_wait(channel);
    if (status < 0 || (status & (ATA_SR_ERR | ATA_SR_DF)) || !(status & ATA_SR_DRQ)) return 0;
    uint16_t identify[256];
    uint32_t done = 0;
    ide_read_data(channel, (uint8_t *)identify, &done, sizeof identify, sizeof identify);
    return (identify[0] >> 14) == 2U && ((identify[0] >> 8) & 0x1FU) == 5U;
}

static void ide_probe_channel(struct ide_channel channel, unsigned number) {
    // No drive pulls the bus down: it floats to 0xFF.
    if (inb(channel.base + IDE_COMMAND) == 0xFFU) return;
    ide_reset(&channel);
    for (unsigned slave = 0; slave < 2U; slave++) {
        struct ide_atapi probe = {channel, (uint8_t)(0xA0U | slave << 4)};
        outb(channel.base + IDE_SELECT, probe.select);
        ide_delay(&channel);
        // The signature a packet device leaves after the reset (ATA/ATAPI-6, 9.12).
        if (inb(channel.base + IDE_BYTES_LOW) != 0x14U ||
            inb(channel.base + IDE_BYTES_HIGH) != 0xEBU)
            continue;
        if (!ide_is_optical(&probe)) continue;
        struct ide_atapi *drive = kmalloc(sizeof(*drive));
        if (!drive) return;
        *drive = probe;
        kprintf("IDE: %s %s is an optical drive\n", number ? "secondary" : "primary",
                slave ? "slave" : "master");
        if (cdrom_add("ide-cd", ide_packet, drive, IDE_PACKET_MAX) != 0) kfree(drive);
    }
}

// The channels: a controller in native mode says where in its BARs (PCI IDE
// 1.0, 2), one in compatibility mode, or none at all, has them at the ISA ports.
static void ide_probe_optical(void) {
    static const struct ide_channel legacy[2] = {{0x1F0U, 0x3F6U, 0}, {0x170U, 0x376U, 0}};
    int legacy_done = 0;
    struct pci_device controller;
    for (unsigned nth = 0; pci_find_nth_class(0x01U, 0x01U, nth, &controller) == 0; nth++) {
        pci_enable_bus_mastering(&controller);
        // BAR4: eight bus master registers a channel, if the controller can DMA.
        uint32_t bar4 = controller.bar[4];
        uint32_t bus_master = (bar4 & 0x01U) && (bar4 & ~0x03U) <= 0xFFF0U ? bar4 & ~0x03U : 0;
        for (unsigned number = 0; number < 2U; number++) {
            uint16_t master = bus_master ? (uint16_t)(bus_master + number * 8U) : 0;
            if (controller.prog_if & (1U << (number * 2U))) {
                uint32_t base = controller.bar[number * 2U] & ~0x03U;
                uint32_t control = controller.bar[number * 2U + 1U] & ~0x03U;
                if (!base || !control || base > 0xFFF8U || control > 0xFFF8U) continue;
                ide_probe_channel(
                    (struct ide_channel){(uint16_t)base, (uint16_t)(control + 2U), master}, number);
            } else if (!(legacy_done & (1 << number))) {
                legacy_done |= 1 << number;
                struct ide_channel channel = legacy[number];
                channel.bus_master = master;
                ide_probe_channel(channel, number);
            }
        }
    }
    if (!legacy_done) {
        ide_probe_channel(legacy[0], 0);
        ide_probe_channel(legacy[1], 1);
    }
}

void ata_register_block_device(void) {
    // First, as it resets the channels: the disk is identified after.
    ide_probe_optical();
    uint32_t sectors = ata_disk_sectors();
    if (!sectors) return;
    struct block_device device;
    memset(&device, 0, sizeof(device));
    device.name[0] = 'i';
    device.name[1] = 'd';
    device.name[2] = 'e';
    device.name[3] = '0';
    device.sectors = sectors;
    device.read = ata_block_read;
    device.write = ata_block_write;
    device.flush = ata_block_flush;
    block_register(&device);
}
