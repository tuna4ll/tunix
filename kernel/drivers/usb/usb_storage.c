#include <stddef.h>
#include <stdint.h>
#include "../../include/block.h"
#include "../../include/dma.h"
#include "../../include/kstring.h"
#include "../../include/pmm.h"
#include "../../include/usb_storage.h"
#include "../../include/vmm.h"
#include "../../include/usb.h"

/* USB mass storage: a 31-byte command wrapper out, the data, a 13-byte status
   wrapper in, all through one physically contiguous staging page. */

extern void kprintf(const char *fmt, ...);

#define CBW_SIGNATURE 0x43425355U   /* "USBC" */
#define CSW_SIGNATURE 0x53425355U   /* "USBS" */
#define CBW_FLAG_IN 0x80U

#define SCSI_TEST_UNIT_READY 0x00U
#define SCSI_REQUEST_SENSE 0x03U
#define SCSI_INQUIRY 0x12U
#define SCSI_READ_CAPACITY_10 0x25U
#define SCSI_READ_10 0x28U
#define SCSI_WRITE_10 0x2AU

#define USB_STORAGE_MAX 4
/* One page of staging is 8 sectors of 512 bytes. */
/* Four pages, because a transfer costs far more than the bytes in it. */
/* Every SCSI command is three polled transfers -- the wrapper out, the data,
   the status back -- and the driver spins on each one with the kernel lock
   held, so a read split into four commands stops the machine four times. */
/* Four rather than more because EHCI describes a transfer with five page
   pointers and nothing larger fits in one qTD; xHCI would take more, and the
   smaller of the two is what a single buffer can promise. */
#define STAGING_BYTES 16384U
#define STAGING_SECTORS (STAGING_BYTES / BLOCK_SECTOR_SIZE)
/* Writes stay at one page, because the larger transfer buys nothing there and
   costs something real. */
/* A stick answers the status phase only once it has committed the data, and it
   NAKs until then -- which is not an error, so the transfer stays active until
   it times out. Four pages of flash take longer to commit than one, and on a
   Core i5 M 430 booting from a stick that was long enough: every WRITE_10 in
   runit's second stage timed out four times over, eight seconds of the kernel
   lock each, and the machine never reached weston. */
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

/* One page each, allocated once: the wrappers and the data all need addresses
   the controller can reach, and allocating per request would fail exactly when
   memory is short and the disk is most needed. */
static uint8_t *wrapper_page;
static uint64_t wrapper_physical;
static uint8_t *staging_page;
static uint64_t staging_physical;
static uint32_t next_tag = 1;

/* Run one SCSI command. Zero is success, TRANSPORT_FAILED a transfer that did
   not complete, REJECTED the device answering properly to say no. */
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
    /* Status two is a phase error, which the specification answers with the
       reset the retry already does. One is the device saying no. */
    return csw->status == 2U ? TRANSPORT_FAILED : REJECTED;
}

/* The same, retried with a class reset in between: a command that failed after
   its wrapper went out leaves the device waiting mid-transaction. */
#define COMMAND_ATTEMPTS 4
/* Enough to say a disk is failing, not enough to bury the log -- and the log
   is painted on the console, so a message per failed block is not a diagnostic
   but a second failure on top of the first. */
#define COMMAND_REPORTS 8U

/* How many commands may fail in a row before the retries are given up on: each
   attempt is a two-second timeout and a reset with the kernel lock held. */
#define FAILURES_BEFORE_BACKING_OFF 3U

static unsigned consecutive_failures;

static int run_command(struct usb_disk *disk, const uint8_t *command,
                       uint8_t command_length, int in, uint32_t length) {
    static unsigned reported;
    int backed_off = consecutive_failures >= FAILURES_BEFORE_BACKING_OFF;
    int attempts = backed_off ? 1 : COMMAND_ATTEMPTS;

    for (int attempt = 0; attempt < attempts; attempt++) {
        /* The reset is what puts a device that stalled mid-command back in a
           state where anything works, so backing off must not skip it: one
           reset and one attempt per command, rather than none at all. */
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
        /*
         * The device answered and said no, so there is nothing to recover:
         * resetting it and asking three more times is four seconds of a held
         * kernel lock to be told the same thing again. A write-protected stick
         * spent that on every block a filesystem tried to flush.
         */
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

/* --- the block layer's view ---------------------------------------------- */

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
    /* The medium counts in its own block size; the block layer counts in 512s,
       and a stick formatted with 2 KiB blocks is not unusual. */
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

/* --- bring-up ------------------------------------------------------------ */

/*
 * A device that has just been configured answers the first command with "unit
 * attention" rather than doing it, which is its way of saying the medium may
 * have changed since anyone last looked. The answer is to ask again.
 */
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

    /* The answer is the address of the *last* block, not how many there are. */
    uint32_t last = get_be32(staging_page);
    uint32_t block_bytes = get_be32(staging_page + 4);
    if (!block_bytes || block_bytes % BLOCK_SECTOR_SIZE || last == 0xFFFFFFFFU)
        return -1;

    disk->block_bytes = block_bytes;
    disk->sectors_per_block = block_bytes / BLOCK_SECTOR_SIZE;
    disk->sectors = ((uint64_t)last + 1U) * disk->sectors_per_block;
    return 0;
}

void usb_storage_init(void) {
    int present = usb_storage_count();
    if (!present) return;

    wrapper_page = (uint8_t *)vmm_phys_to_virt((wrapper_physical = (uint64_t)pmm_alloc_page()));
    staging_page = (uint8_t *)dma_alloc(STAGING_BYTES, 4096, &staging_physical);
    if (!wrapper_physical || !staging_page || !staging_physical) return;
    memset(wrapper_page, 0, 4096);

    for (int index = 0; index < present && disk_count < USB_STORAGE_MAX; index++) {
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
            continue;
        }
        if (wait_until_ready(disk) != 0) {
            kprintf("USB-STORAGE: device %d never became ready\n", index);
            continue;
        }
        if (read_capacity(disk) != 0) {
            kprintf("USB-STORAGE: device %d has no readable capacity\n", index);
            continue;
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
        if (block_register(&device) < 0) continue;
        disk_count++;
    }
}
