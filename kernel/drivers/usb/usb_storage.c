#include <stddef.h>
#include <stdint.h>
#include "../../include/block.h"
#include "../../include/devfs.h"
#include "../../include/partition.h"
#include "../../include/dma.h"
#include "../../include/kstring.h"
#include "../../include/pmm.h"
#include "../../include/usb_storage.h"
#include "../../include/vmm.h"
#include "../../include/usb.h"

extern void kprintf(const char *fmt, ...);

#define CBW_SIGNATURE 0x43425355U
#define CSW_SIGNATURE 0x53425355U
#define CBW_FLAG_IN 0x80U

#define SCSI_TEST_UNIT_READY 0x00U
#define SCSI_REQUEST_SENSE 0x03U
#define SCSI_INQUIRY 0x12U
#define SCSI_READ_CAPACITY_10 0x25U
#define SCSI_READ_10 0x28U
#define SCSI_WRITE_10 0x2AU

#define USB_STORAGE_MAX 4
#define STAGING_BYTES 16384U
#define STAGING_SECTORS (STAGING_BYTES / BLOCK_SECTOR_SIZE)
#define STAGING_WRITE_SECTORS (4096U / BLOCK_SECTOR_SIZE)

struct command_block_wrapper {
    uint32_t signature;
    uint32_t tag;
    uint32_t transfer_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t command_length;
    uint8_t command[16];
} __attribute__((packed));

struct command_status_wrapper {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} __attribute__((packed));

struct usb_disk {
    int used;
    int controller_index;
    uint64_t sectors;
    uint32_t block_bytes;
    uint32_t sectors_per_block;
    char name[16];
};

static struct usb_disk disks[USB_STORAGE_MAX];
static int disk_count;

static uint8_t *wrapper_page;
static uint64_t wrapper_physical;
static uint8_t *staging_page;
static uint64_t staging_physical;
static uint32_t next_tag = 1;
static int initialized;

#define TRANSPORT_FAILED (-1)
#define REJECTED (-2)

static int run_command_once(struct usb_disk *disk, const uint8_t *command,
                            uint8_t command_length, int in, uint32_t length) {
    struct command_block_wrapper *cbw = (struct command_block_wrapper *)wrapper_page;
    memset(cbw, 0, sizeof(*cbw));
    cbw->signature = CBW_SIGNATURE;
    cbw->tag = next_tag++;
    cbw->transfer_length = length;
    cbw->flags = in ? CBW_FLAG_IN : 0;
    cbw->lun = 0;
    cbw->command_length = command_length;
    memcpy(cbw->command, command, command_length);
    uint32_t tag = cbw->tag;

    if (usb_bulk_transfer(disk->controller_index, 0, wrapper_physical,
                           sizeof(*cbw)) != 0) return TRANSPORT_FAILED;

    if (length &&
        usb_bulk_transfer(disk->controller_index, in, staging_physical, length) != 0)
        return TRANSPORT_FAILED;

    struct command_status_wrapper *csw =
        (struct command_status_wrapper *)(wrapper_page + 64);
    memset(csw, 0, sizeof(*csw));
    if (usb_bulk_transfer(disk->controller_index, 1, wrapper_physical + 64,
                           sizeof(*csw)) != 0) return TRANSPORT_FAILED;

    if (csw->signature != CSW_SIGNATURE || csw->tag != tag) return TRANSPORT_FAILED;
    if (csw->status == 0) return 0;
    return csw->status == 2U ? TRANSPORT_FAILED : REJECTED;
}

#define COMMAND_ATTEMPTS 4
#define COMMAND_REPORTS 8U

#define FAILURES_BEFORE_BACKING_OFF 3U

static unsigned consecutive_failures;

static int run_command(struct usb_disk *disk, const uint8_t *command,
                       uint8_t command_length, int in, uint32_t length) {
    static unsigned reported;
    if (!usb_storage_present(disk->controller_index)) return -1;
    int backed_off = consecutive_failures >= FAILURES_BEFORE_BACKING_OFF;
    int attempts = backed_off ? 1 : COMMAND_ATTEMPTS;

    for (int attempt = 0; attempt < attempts; attempt++) {
        if (!usb_storage_present(disk->controller_index)) return -1;
        if ((attempt || backed_off) &&
            usb_reset_recovery(disk->controller_index) != 0) break;
        int status = run_command_once(disk, command, command_length, in, length);
        if (status == 0) {
            if (attempt && reported < COMMAND_REPORTS) {
                reported++;
                kprintf("USB-STORAGE: command %x needed %d attempts\n",
                        (unsigned)command[0], attempt + 1);
            }
            consecutive_failures = 0;
            return 0;
        }
        if (status == REJECTED) {
            if (reported < COMMAND_REPORTS) {
                reported++;
                kprintf("USB-STORAGE: the device refused command %x\n",
                        (unsigned)command[0]);
            }
            consecutive_failures++;
            return -1;
        }
    }
    consecutive_failures++;
    if (reported < COMMAND_REPORTS) {
        reported++;
        kprintf("USB-STORAGE: command %x failed %d times%s\n",
                (unsigned)command[0], attempts,
                consecutive_failures >= FAILURES_BEFORE_BACKING_OFF
                    ? ", retries given up until one works" : "");
    }
    return -1;
}

static void put_be32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint32_t get_be32(const uint8_t *in) {
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | in[3];
}

static int transfer_sectors(struct usb_disk *disk, uint64_t lba, uint32_t count,
                            void *buffer, int write) {
    if (lba % disk->sectors_per_block || count % disk->sectors_per_block) return -1;
    uint32_t block = (uint32_t)(lba / disk->sectors_per_block);
    uint32_t blocks = count / disk->sectors_per_block;

    uint8_t command[10];
    memset(command, 0, sizeof(command));
    command[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
    put_be32(command + 2, block);
    command[7] = (uint8_t)(blocks >> 8);
    command[8] = (uint8_t)blocks;

    uint32_t bytes = count * BLOCK_SECTOR_SIZE;
    if (write) memcpy(staging_page, buffer, bytes);
    if (run_command(disk, command, sizeof(command), write ? 0 : 1, bytes) != 0)
        return -1;
    if (!write) memcpy(buffer, staging_page, bytes);
    return 0;
}

static int usb_read(void *context, uint64_t lba, uint32_t count, void *destination) {
    struct usb_disk *disk = (struct usb_disk *)context;
    uint8_t *out = (uint8_t *)destination;
    while (count) {
        uint32_t chunk = count > STAGING_SECTORS ? STAGING_SECTORS : count;
        if (chunk % disk->sectors_per_block)
            chunk -= chunk % disk->sectors_per_block;
        if (!chunk) return -1;
        if (transfer_sectors(disk, lba, chunk, out, 0) != 0) return -1;
        out += (size_t)chunk * BLOCK_SECTOR_SIZE;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int usb_write(void *context, uint64_t lba, uint32_t count, const void *source) {
    struct usb_disk *disk = (struct usb_disk *)context;
    const uint8_t *in = (const uint8_t *)source;
    while (count) {
        uint32_t chunk = count > STAGING_WRITE_SECTORS ? STAGING_WRITE_SECTORS : count;
        if (chunk % disk->sectors_per_block)
            chunk -= chunk % disk->sectors_per_block;
        if (!chunk) return -1;
        if (transfer_sectors(disk, lba, chunk, (void *)(uintptr_t)in, 1) != 0) return -1;
        in += (size_t)chunk * BLOCK_SECTOR_SIZE;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int wait_until_ready(struct usb_disk *disk) {
    uint8_t command[6];
    for (int attempt = 0; attempt < 16; attempt++) {
        memset(command, 0, sizeof(command));
        command[0] = SCSI_TEST_UNIT_READY;
        if (run_command(disk, command, sizeof(command), 0, 0) == 0) return 0;

        memset(command, 0, sizeof(command));
        command[0] = SCSI_REQUEST_SENSE;
        command[4] = 18;
        (void)run_command(disk, command, sizeof(command), 1, 18);
    }
    return -1;
}

static int read_capacity(struct usb_disk *disk) {
    uint8_t command[10];
    memset(command, 0, sizeof(command));
    command[0] = SCSI_READ_CAPACITY_10;
    if (run_command(disk, command, sizeof(command), 1, 8) != 0) return -1;

    uint32_t last = get_be32(staging_page);
    uint32_t block_bytes = get_be32(staging_page + 4);
    if (!block_bytes || block_bytes % BLOCK_SECTOR_SIZE || last == 0xFFFFFFFFU)
        return -1;

    disk->block_bytes = block_bytes;
    disk->sectors_per_block = block_bytes / BLOCK_SECTOR_SIZE;
    disk->sectors = ((uint64_t)last + 1U) * disk->sectors_per_block;
    return 0;
}

static int ensure_pages(void) {
    if (wrapper_page && staging_page) return 0;
    wrapper_physical = (uint64_t)pmm_alloc_page();
    if (!wrapper_physical) return -1;
    wrapper_page = (uint8_t *)vmm_phys_to_virt(wrapper_physical);
    staging_page = (uint8_t *)dma_alloc(STAGING_BYTES, 4096, &staging_physical);
    if (!wrapper_page || !staging_page || !staging_physical) return -1;
    memset(wrapper_page, 0, 4096);
    return 0;
}

static int attach_disk(int index) {
    if (disk_count >= USB_STORAGE_MAX || ensure_pages() != 0) return -1;
    struct usb_disk *disk = &disks[disk_count];
    memset(disk, 0, sizeof(*disk));
    disk->controller_index = index;
    disk->sectors_per_block = 1;

    uint8_t command[6];
    memset(command, 0, sizeof(command));
    command[0] = SCSI_INQUIRY;
    command[4] = 36;
    if (run_command(disk, command, sizeof(command), 1, 36) != 0) {
        kprintf("USB-STORAGE: device %d did not answer INQUIRY\n", index);
        return -1;
    }
    if (wait_until_ready(disk) != 0) {
        kprintf("USB-STORAGE: device %d never became ready\n", index);
        return -1;
    }
    if (read_capacity(disk) != 0) {
        kprintf("USB-STORAGE: device %d has no readable capacity\n", index);
        return -1;
    }

    disk->used = 1;
    struct block_device device;
    memset(&device, 0, sizeof(device));
    device.name[0] = 'u'; device.name[1] = 's'; device.name[2] = 'b';
    device.name[3] = (char)('0' + disk_count);
    device.sectors = disk->sectors;
    device.read = usb_read;
    device.write = usb_write;
    device.flush = NULL;
    device.context = disk;
    int registered = block_register(&device);
    if (registered < 0) {
        disk->used = 0;
        return -1;
    }
    disk_count++;
    return registered;
}

void usb_storage_init(void) {
    int present = usb_storage_count();
    for (int index = 0; index < present; index++) (void)attach_disk(index);
    initialized = 1;
}

void usb_storage_attach(int index) {
    if (!initialized) return;
    int before = block_device_count();
    int registered = attach_disk(index);
    if (registered < 0) return;
    partition_scan_disk(registered);
    int after = block_device_count();
    for (int device = before; device < after; device++) devfs_add_block(device);
}
