#include <stddef.h>
#include <stdint.h>
#include <tunix/block.h>
#include <tunix/cdrom.h>
#include <tunix/devfs.h>
#include <tunix/dma.h>
#include <tunix/eventfs.h>
#include <tunix/heap.h>
#include <tunix/kprintf.h>
#include <tunix/kstring.h>
#include <tunix/lock.h>
#include <tunix/mutex.h>
#include <tunix/partition.h>
#include <tunix/process.h>
#include <tunix/time.h>

// Optical drives, whatever bus they are on: their 2048-byte blocks read as the
// block layer's 512-byte sectors, and their discs watched as they come and go.

#define CDROM_MAX      16U
#define CDROM_BLOCK    2048U
#define BLOCK_MAJOR_SR 11U
#define POLL_NS        (2ULL * 1000000000ULL)

#define SCSI_TEST_UNIT_READY 0x00U
#define SCSI_REQUEST_SENSE   0x03U
#define SCSI_PREVENT_REMOVAL 0x1EU
#define SCSI_READ_CAPACITY   0x25U
#define SCSI_READ_10         0x28U

// A unit attention's "medium may have changed" (SPC-4); a reset's (0x29) leaves
// the disc as it was.
#define ASC_MEDIUM_CHANGED 0x28U

struct cdrom_drive {
    cdrom_packet_fn packet;
    void *context;
    uint32_t max_blocks;
    // One block, below 4 GiB for any DMA engine: the ends of unaligned reads,
    // and the drive's short replies.
    uint8_t *bounce;
    uint32_t block_bytes;
    uint32_t block_sectors;
    uint64_t sectors;
    int present;
    // The disc changed and the block layer has not caught up: reads fail, so
    // that nothing reads the new disc as the old one.
    int changed;
    int locked;
    int index;
    struct mutex lock;
};

static struct cdrom_drive *drives[CDROM_MAX];
static unsigned drive_count;
static const char poll_channel;

static int command(struct cdrom_drive *drive, const uint8_t *packet, void *buffer,
                   uint32_t bytes) {
    return drive->packet(drive->context, packet, buffer, bytes);
}

// What the unit attention just reported was about; when the drive will not
// say, it is taken for a change, which loses nothing but an open file.
static int medium_changed(struct cdrom_drive *drive) {
    static const uint8_t packet[12] = {SCSI_REQUEST_SENSE, 0, 0, 0, 18};
    memset(drive->bounce, 0, 18);
    if (command(drive, packet, drive->bounce, 18) != 0) return 1;
    return drive->bounce[12] == ASC_MEDIUM_CHANGED;
}

// TEST UNIT READY past the unit attentions a drive reports once each: 0 when a
// disc is ready, otherwise as the packet function returns.
static int test_ready(struct cdrom_drive *drive) {
    static const uint8_t packet[12] = {SCSI_TEST_UNIT_READY};
    for (int attempt = 0; attempt < 4; attempt++) {
        int status = command(drive, packet, NULL, 0);
        if (status != CDROM_SENSE_UNIT_ATTENTION) return status;
        if (medium_changed(drive)) drive->changed = 1;
    }
    return -1;
}

static int read_capacity(struct cdrom_drive *drive) {
    static const uint8_t packet[12] = {SCSI_READ_CAPACITY};
    uint8_t *reply = drive->bounce;
    if (command(drive, packet, reply, 8) != 0) return -1;
    uint32_t last = (uint32_t)reply[0] << 24 | (uint32_t)reply[1] << 16 |
        (uint32_t)reply[2] << 8 | reply[3];
    uint32_t bytes = (uint32_t)reply[4] << 24 | (uint32_t)reply[5] << 16 |
        (uint32_t)reply[6] << 8 | reply[7];
    // Some drives report 0, or 2340 for an audio CD; data discs are read in 2048.
    if (!bytes || bytes > CDROM_BLOCK || bytes % BLOCK_SECTOR_SIZE) bytes = CDROM_BLOCK;
    drive->block_bytes = bytes;
    drive->block_sectors = bytes / BLOCK_SECTOR_SIZE;
    drive->sectors = ((uint64_t)last + 1U) * drive->block_sectors;
    return 0;
}

static int read_blocks(struct cdrom_drive *drive, uint32_t block, uint32_t count, void *out) {
    uint8_t packet[12] = {SCSI_READ_10,          0, (uint8_t)(block >> 24), (uint8_t)(block >> 16),
                          (uint8_t)(block >> 8), (uint8_t)block, 0, (uint8_t)(count >> 8),
                          (uint8_t)count,        0, 0, 0};
    for (int attempt = 0; attempt < 2; attempt++) {
        int status = command(drive, packet, out, count * drive->block_bytes);
        if (status != CDROM_SENSE_UNIT_ATTENTION) return status ? -1 : 0;
        if (medium_changed(drive)) {
            drive->changed = 1;
            return -1;
        }
    }
    return -1;
}

// Whole blocks go straight to the caller, the ends of an unaligned request
// through the bounce buffer.
static int read_unlocked(struct cdrom_drive *drive, uint64_t lba, uint32_t count, uint8_t *out) {
    uint32_t per = drive->block_sectors;
    while (count) {
        uint64_t block = lba / per;
        uint32_t skip = (uint32_t)(lba % per);
        if (block > 0xFFFFFFFFULL) return -1;
        if (skip || count < per) {
            if (read_blocks(drive, (uint32_t)block, 1, drive->bounce) != 0) return -1;
            uint32_t take = per - skip < count ? per - skip : count;
            memcpy(out, drive->bounce + skip * BLOCK_SECTOR_SIZE, take * BLOCK_SECTOR_SIZE);
            out += take * BLOCK_SECTOR_SIZE;
            lba += take;
            count -= take;
            continue;
        }
        uint32_t blocks = count / per;
        if (blocks > drive->max_blocks) blocks = drive->max_blocks;
        if (read_blocks(drive, (uint32_t)block, blocks, out) != 0) return -1;
        out += blocks * per * BLOCK_SECTOR_SIZE;
        lba += (uint64_t)blocks * per;
        count -= blocks * per;
    }
    return 0;
}

static int cdrom_read(void *context, uint64_t lba, uint32_t count, void *destination) {
    struct cdrom_drive *drive = context;
    mutex_lock(&drive->lock);
    int status = -1;
    if (drive->present && !drive->changed && lba <= drive->sectors &&
        count <= drive->sectors - lba)
        status = read_unlocked(drive, lba, count, destination);
    mutex_unlock(&drive->lock);
    return status;
}

// Brings the drive's idea of its disc up to date: 1 when a disc came, went or
// was swapped, and the block layer's view of the old one is already dropped.
static int poll_unlocked(struct cdrom_drive *drive) {
    int status = test_ready(drive);
    int was = drive->present;
    if (status == 0) {
        if (was && !drive->changed) return 0;
        drive->present = read_capacity(drive) == 0;
    } else if (status == CDROM_SENSE_NOT_READY) {
        // Empty, or still spinning up: either way there is no disc to read yet.
        if (!was && !drive->changed) return 0;
        drive->present = 0;
    } else {
        // The drive did not answer; leave the disc be rather than drop it.
        return 0;
    }
    if (!drive->present) drive->sectors = 0;
    if (!was && !drive->present) {
        drive->changed = 0;
        return 0;
    }
    // shortcut: srN itself stays one entry, so a file system mounted on the
    // whole drive, not a partition, reads the next disc; a new entry per disc
    // would fix that, once a file system for bare discs (ISO 9660) is here.
    block_drop_partitions(drive->index);
    block_set_sectors(drive->index, drive->sectors);
    drive->changed = 0;
    return 1;
}

// The new disc's partitions, found and published like the boot's.
static void publish(struct cdrom_drive *drive, uint64_t sectors) {
    const struct block_device *disk = block_device_at(drive->index);
    kprintf("CDROM: %s: %s, %u sectors\n", disk->dev_name, sectors ? "disc in" : "no disc",
            (unsigned)sectors);
    if (sectors) partition_scan_disk(drive->index);
    int count = block_device_count();
    for (int index = 0; index < count; index++) {
        const struct block_device *device = block_device_at(index);
        if (index == drive->index || (device && device->parent == drive->index))
            devfs_add_block(index);
    }
    eventfs_emit_device_change("block", disk->dev_name);
}

static void poll_drive(struct cdrom_drive *drive) {
    mutex_lock(&drive->lock);
    int event = poll_unlocked(drive);
    uint64_t sectors = drive->sectors;
    mutex_unlock(&drive->lock);
    if (event) publish(drive, sectors);
}

void cdrom_poll(void) {
    unsigned count = __atomic_load_n(&drive_count, __ATOMIC_ACQUIRE);
    for (unsigned index = 0; index < count; index++) poll_drive(drives[index]);
}

// A drive with the root on it keeps its tray shut, as Linux keeps a mounted
// disc's: ejecting the live system's disc would leave it nothing to run.
static void lock_if_root(struct cdrom_drive *drive) {
    static const uint8_t packet[12] = {SCSI_PREVENT_REMOVAL, 0, 0, 0, 1};
    const struct block_device *root = block_root();
    if (drive->locked || !root) return;
    if (root != block_device_at(drive->index) && root->parent != drive->index) return;
    mutex_lock(&drive->lock);
    drive->locked = command(drive, packet, NULL, 0) == 0;
    mutex_unlock(&drive->lock);
    if (drive->locked) kprintf("CDROM: %s holds the root, its tray is locked\n", root->dev_name);
}

static void poller(void *unused) {
    (void)unused;
    for (;;) {
        unsigned count = __atomic_load_n(&drive_count, __ATOMIC_ACQUIRE);
        for (unsigned index = 0; index < count; index++) {
            lock_if_root(drives[index]);
            poll_drive(drives[index]);
        }
        process_prepare_wait(&poll_channel, time_uptime_ns() + POLL_NS);
        process_wait();
        process_finish_wait();
    }
}

void cdrom_start(void) {
    if (!__atomic_load_n(&drive_count, __ATOMIC_ACQUIRE)) return;
    if (!process_create_kthread("kcdrom", poller, NULL))
        kprintf("CDROM: cannot watch the drives for discs\n");
}

int cdrom_add(const char *driver, cdrom_packet_fn packet, void *context, uint32_t max_bytes) {
    if (!packet || drive_count == CDROM_MAX) return -1;
    struct cdrom_drive *drive = kmalloc(sizeof(*drive));
    if (!drive) return -1;
    memset(drive, 0, sizeof(*drive));
    uint64_t physical;
    drive->bounce = dma_alloc_below(4096, 4096, DMA_LIMIT_32BIT, &physical);
    if (!drive->bounce) {
        kfree(drive);
        return -1;
    }
    drive->packet = packet;
    drive->context = context;
    drive->max_blocks = max_bytes / CDROM_BLOCK ? max_bytes / CDROM_BLOCK : 1U;
    drive->block_bytes = CDROM_BLOCK;
    drive->block_sectors = CDROM_BLOCK / BLOCK_SECTOR_SIZE;
    mutex_init(&drive->lock, "cdrom", LOCK_RANK_BLOCK);

    // A disc still spinning up is found later, by cdrom_poll.
    drive->present = test_ready(drive) == 0 && read_capacity(drive) == 0;
    if (!drive->present) drive->sectors = 0;
    drive->changed = 0;

    struct block_device device;
    memset(&device, 0, sizeof(device));
    strncpy(device.name, driver, sizeof device.name - 1);
    device.sectors = drive->sectors;
    device.read = cdrom_read;
    device.context = drive;
    char name[8] = "sr";
    unsigned number = drive_count, length = 2;
    if (number >= 10U) name[length++] = (char)('0' + number / 10U);
    name[length++] = (char)('0' + number % 10U);
    name[length] = '\0';
    drive->index = block_register_named(&device, name, BLOCK_MAJOR_SR, number);
    if (drive->index < 0) {
        dma_free(drive->bounce, 4096);
        kfree(drive);
        return -1;
    }
    drives[drive_count] = drive;
    __atomic_store_n(&drive_count, drive_count + 1U, __ATOMIC_RELEASE);
    return 0;
}
