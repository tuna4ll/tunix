#include <stddef.h>
#include <stdint.h>
#include <tunix/ahci.h>
#if defined(__x86_64__)
#include <tunix/ata.h>
#endif
#include <tunix/block.h>
#include <tunix/eventfs.h>
#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/lock.h>
#include <tunix/nvme.h>
#include <tunix/partition.h>
#include <tunix/time.h>
#include <tunix/usb_storage.h>

extern void kprintf(const char *fmt, ...);

static struct block_device **devices;
static int device_capacity;
static int device_count;
static int disk_count;
static int root_index;

static struct lock registry_lock = LOCK_INITIALIZER("block devices", LOCK_RANK_REGISTRY);

static void registry_guard_release(int *unused) {
    (void)unused;
    lock_release(&registry_lock);
}

#define REGISTRY_LOCKED \
    __attribute__((cleanup(registry_guard_release))) int registry_guard = \
        (lock_acquire(&registry_lock), 0)

struct partition_context {
    int parent;
    uint64_t start;
};

static int partition_read(void *context, uint64_t lba, uint32_t count, void *destination) {
    const struct partition_context *part = context;
    const struct block_device *disk = block_device_at(part->parent);
    if (!disk) return -1;
    return disk->read(disk->context, part->start + lba, count, destination);
}

static int partition_write(void *context, uint64_t lba, uint32_t count, const void *source) {
    const struct partition_context *part = context;
    const struct block_device *disk = block_device_at(part->parent);
    if (!disk || !disk->write) return -1;
    return disk->write(disk->context, part->start + lba, count, source);
}

static int partition_flush(void *context) {
    const struct partition_context *part = context;
    const struct block_device *disk = block_device_at(part->parent);
    if (!disk) return -1;
    return disk->flush ? disk->flush(disk->context) : 0;
}

static void announce(int index) {
    kprintf("BLOCK: %s (%s), %u sectors%s\n", devices[index]->dev_name, devices[index]->name,
            (unsigned)devices[index]->sectors, devices[index]->write ? "" : ", read only");
}

static struct block_device *new_entry(void) {
    if (device_count == device_capacity) {
        int capacity = device_capacity ? device_capacity * 2 : 16;
        struct block_device **grown = kmalloc((size_t)capacity * sizeof(*grown));
        if (!grown) return NULL;
        if (device_count) memcpy(grown, devices, (size_t)device_count * sizeof(*grown));
        kfree(devices);
        devices = grown;
        device_capacity = capacity;
    }
    struct block_device *entry = kmalloc(sizeof(*entry));
    if (!entry) return NULL;
    memset(entry, 0, sizeof(*entry));
    devices[device_count] = entry;
    return entry;
}

static void disk_name(int number, char *out) {
    char letters[8];
    int count = 0;
    do {
        letters[count++] = (char)('a' + number % 26);
        number = number / 26 - 1;
    } while (number >= 0 && count < 6);
    out[0] = 's';
    out[1] = 'd';
    for (int index = 0; index < count; index++) out[2 + index] = letters[count - 1 - index];
    out[2 + count] = '\0';
}

int block_register(const struct block_device *device) {
    if (!device || !device->read || !device->sectors) return -1;
    lock_acquire(&registry_lock);
    struct block_device *entry = new_entry();
    if (!entry) {
        lock_release(&registry_lock);
        return -1;
    }
    *entry = *device;
    entry->name[sizeof entry->name - 1] = '\0';
    entry->parent = -1;
    disk_name(disk_count, entry->dev_name);
    disk_count++;
    announce(device_count);
    int index = device_count;
    __atomic_store_n(&device_count, index + 1, __ATOMIC_RELEASE);
    lock_release(&registry_lock);
    eventfs_emit_device_attach("block", entry->dev_name);
    return index;
}

int block_register_partition(int parent, int number, uint64_t start, uint64_t sectors) {
    const struct block_device *disk = block_device_at(parent);
    if (!disk || disk->parent != -1 || number < 1 || number > 99999) return -1;
    if (!sectors || start >= disk->sectors || sectors > disk->sectors - start) return -1;

    struct partition_context *context = kmalloc(sizeof(*context));
    if (!context) return -1;
    context->parent = parent;
    context->start = start;

    lock_acquire(&registry_lock);
    struct block_device *entry = new_entry();
    if (!entry) {
        lock_release(&registry_lock);
        kfree(context);
        return -1;
    }
    memcpy(entry->name, disk->name, sizeof entry->name);
    memcpy(entry->dev_name, disk->dev_name, sizeof entry->dev_name);
    size_t length = strlen(entry->dev_name);
    char digits[8];
    int count = 0;
    do {
        digits[count++] = (char)('0' + number % 10);
        number /= 10;
    } while (number);
    while (count) entry->dev_name[length++] = digits[--count];
    entry->dev_name[length] = '\0';
    entry->parent = parent;
    entry->sectors = sectors;
    entry->read = partition_read;
    entry->write = disk->write ? partition_write : NULL;
    entry->flush = partition_flush;
    entry->context = context;
    announce(device_count);
    int index = device_count;
    __atomic_store_n(&device_count, index + 1, __ATOMIC_RELEASE);
    lock_release(&registry_lock);
    eventfs_emit_device_attach("block", entry->dev_name);
    return index;
}

int block_device_index_by_name(const char *name) {
    if (!name) return -1;
    if (name[0] == '/') {
        static const char prefix[] = "/dev/";
        for (size_t index = 0; index < sizeof prefix - 1; index++)
            if (name[index] != prefix[index]) return -1;
        name += sizeof prefix - 1;
    }
    REGISTRY_LOCKED;
    for (int index = 0; index < device_count; index++)
        if (strcmp(devices[index]->dev_name, name) == 0) return index;
    return -1;
}

int block_device_count(void) { return __atomic_load_n(&device_count, __ATOMIC_ACQUIRE); }

const struct block_device *block_device_at(int index) {
    REGISTRY_LOCKED;
    if (index < 0 || index >= device_count) return NULL;
    return devices[index];
}

const struct block_device *block_root(void) {
    REGISTRY_LOCKED;
    return root_index < device_count ? devices[root_index] : NULL;
}

void block_select_root(int index) {
    REGISTRY_LOCKED;
    root_index = index >= 0 && index < device_count ? index : 0;
    if (device_count) kprintf("BLOCK: root on %s\n", devices[root_index]->dev_name);
}

static uint64_t block_reads;
static uint64_t block_sectors_read;
static uint64_t block_read_ns;
static uint64_t block_write_failures;
static uint64_t block_writes;
static uint64_t block_sectors_written;
static uint64_t block_write_ns;

void block_write_statistics(uint64_t *writes, uint64_t *sectors, uint64_t *nanoseconds) {
    if (writes) *writes = __atomic_load_n(&block_writes, __ATOMIC_RELAXED);
    if (sectors) *sectors = __atomic_load_n(&block_sectors_written, __ATOMIC_RELAXED);
    if (nanoseconds) *nanoseconds = __atomic_load_n(&block_write_ns, __ATOMIC_RELAXED);
}

void block_statistics(uint64_t *reads, uint64_t *sectors, uint64_t *nanoseconds,
                      uint64_t *write_failures) {
    if (reads) *reads = __atomic_load_n(&block_reads, __ATOMIC_RELAXED);
    if (sectors) *sectors = __atomic_load_n(&block_sectors_read, __ATOMIC_RELAXED);
    if (nanoseconds) *nanoseconds = __atomic_load_n(&block_read_ns, __ATOMIC_RELAXED);
    if (write_failures) *write_failures = __atomic_load_n(&block_write_failures, __ATOMIC_RELAXED);
}

static int device_read_counted(const struct block_device *device, uint64_t lba, uint32_t count,
                               void *destination) {
    uint64_t begun = time_uptime_ns();
    int status = device->read(device->context, lba, count, destination);
    uint64_t now = time_uptime_ns();
    __atomic_fetch_add(&block_reads, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&block_sectors_read, count, __ATOMIC_RELAXED);
    if (now > begun) __atomic_fetch_add(&block_read_ns, now - begun, __ATOMIC_RELAXED);
    return status;
}

int block_device_read(const struct block_device *device, uint64_t lba, uint32_t count,
                      void *destination) {
    if (!device || !device->read || !count || !destination) return -1;
    if (lba + count > device->sectors) return -1;
    return device_read_counted(device, lba, count, destination);
}

#define WRITE_FAILURE_REPORT_LIMIT 8U

int block_device_write(const struct block_device *device, uint64_t lba, uint32_t count,
                       const void *source) {
    if (!device || !device->write || !count || !source) return -1;
    if (lba + count > device->sectors) return -1;
    uint64_t begun = time_uptime_ns();
    int status = device->write(device->context, lba, count, source);
    uint64_t now = time_uptime_ns();
    __atomic_fetch_add(&block_writes, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&block_sectors_written, count, __ATOMIC_RELAXED);
    if (now > begun) __atomic_fetch_add(&block_write_ns, now - begun, __ATOMIC_RELAXED);
    if (status != 0) {
        uint64_t failures = __atomic_add_fetch(&block_write_failures, 1, __ATOMIC_RELAXED);
        if (failures <= WRITE_FAILURE_REPORT_LIMIT)
            kprintf("BLOCK: write of %u sectors at lba %u on %s failed (%d)%s\n", (unsigned)count,
                    (unsigned)lba, device->dev_name, status,
                    failures == WRITE_FAILURE_REPORT_LIMIT
                        ? ", further failures counted in /proc/blockstat"
                        : "");
    }
    return status;
}

int block_device_flush(const struct block_device *device) {
    if (!device) return -1;
    return device->flush ? device->flush(device->context) : 0;
}

int block_device_write_bytes(const struct block_device *device, uint64_t offset, size_t size,
                             const void *source) {
    if (!device || !device->write || !source) return -1;
    const uint8_t *in = (const uint8_t *)source;
    uint8_t sector[BLOCK_SECTOR_SIZE];

    while (size) {
        uint64_t lba = offset / BLOCK_SECTOR_SIZE;
        size_t within = (size_t)(offset % BLOCK_SECTOR_SIZE);
        if (lba >= device->sectors) return -1;

        if (!within && size >= BLOCK_SECTOR_SIZE) {
            uint64_t whole = size / BLOCK_SECTOR_SIZE;
            if (whole > device->sectors - lba) whole = device->sectors - lba;
            if (whole > 0xFFFFU) whole = 0xFFFFU;
            if (device->write(device->context, lba, (uint32_t)whole, in) != 0) return -1;
            size_t moved = (size_t)whole * BLOCK_SECTOR_SIZE;
            in += moved;
            offset += moved;
            size -= moved;
            continue;
        }

        if (device_read_counted(device, lba, 1, sector) != 0) return -1;
        size_t chunk = BLOCK_SECTOR_SIZE - within;
        if (chunk > size) chunk = size;
        memcpy(sector + within, in, chunk);
        if (device->write(device->context, lba, 1, sector) != 0) return -1;
        in += chunk;
        offset += chunk;
        size -= chunk;
    }
    return 0;
}

int block_device_read_bytes(const struct block_device *device, uint64_t offset, size_t size,
                            void *destination) {
    if (!device || !destination) return -1;
    uint8_t *out = (uint8_t *)destination;
    uint8_t sector[BLOCK_SECTOR_SIZE];

    while (size) {
        uint64_t lba = offset / BLOCK_SECTOR_SIZE;
        size_t within = (size_t)(offset % BLOCK_SECTOR_SIZE);
        if (lba >= device->sectors) return -1;

        if (!within && size >= BLOCK_SECTOR_SIZE) {
            uint64_t whole = size / BLOCK_SECTOR_SIZE;
            if (whole > device->sectors - lba) whole = device->sectors - lba;
            if (whole > 0xFFFFU) whole = 0xFFFFU;
            if (device_read_counted(device, lba, (uint32_t)whole, out) != 0) return -1;
            size_t moved = (size_t)whole * BLOCK_SECTOR_SIZE;
            out += moved;
            offset += moved;
            size -= moved;
            continue;
        }

        if (device_read_counted(device, lba, 1, sector) != 0) return -1;
        size_t chunk = BLOCK_SECTOR_SIZE - within;
        if (chunk > size) chunk = size;
        memcpy(out, sector + within, chunk);
        out += chunk;
        offset += chunk;
        size -= chunk;
    }
    return 0;
}

void block_probe(void) {
#if defined(__x86_64__)
    ata_register_block_device();
#endif
    ahci_init();
    nvme_init();
    usb_storage_init();
    partition_scan();
}
