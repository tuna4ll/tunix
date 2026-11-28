// The initramfs: a newc cpio archive the bootloader loads beside the kernel,
// as Linux's is. Tunix mounts its root itself, so what the archive is for is
// the drivers that have to be there before the root is: before the console is
// drawn, the kernel loads every module in it that one of the machine's PCI
// devices asks for by its modalias, as udev would, and those named by
// rd.driver.pre=. A display driver loaded so takes the console from its
// first line, as a built-in one does.

#include <stddef.h>
#include <stdint.h>

#include <tunix/boot.h>
#include <tunix/errno.h>
#include <tunix/heap.h>
#include <tunix/initramfs.h>
#include <tunix/kprintf.h>
#include <tunix/kstring.h>
#include <tunix/module.h>
#include <tunix/pci.h>
#include <tunix/vmm.h>

#define CPIO_HEADER_BYTES 110U
#define MAX_MODULES       64U
#define MAX_DEVICES       128U
#define ALIAS_BYTES       160U
#define MODALIAS_BYTES    64U

struct archive_file {
    const char *name;
    const uint8_t *data;
    uint32_t size;
};

static int hex_field(const uint8_t *text, uint32_t *value) {
    uint32_t result = 0;
    for (unsigned index = 0; index < 8U; index++) {
        uint8_t c = text[index];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10U;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10U;
        else return -1;
        result = (result << 4) | digit;
    }
    *value = result;
    return 0;
}

static uint64_t align4(uint64_t value) { return (value + 3U) & ~3ULL; }

static int ends_with(const char *text, const char *suffix) {
    size_t length = strlen(text), tail = strlen(suffix);
    return length >= tail && strcmp(text + length - tail, suffix) == 0;
}

// The regular files of ARCHIVE whose names end in .ko, in archive order. Each
// archive may be followed by another, as the kernel's own concatenation of
// cpio archives allows; zeros may pad between them.
static unsigned list_modules(const uint8_t *archive, uint64_t size, struct archive_file *out,
                             unsigned capacity) {
    unsigned found = 0;
    uint64_t at = 0;
    while (at + CPIO_HEADER_BYTES <= size) {
        if (archive[at] == 0) {
            at += 4U;
            continue;
        }
        const uint8_t *header = archive + at;
        if (memcmp(header, "070701", 6) != 0 && memcmp(header, "070702", 6) != 0) {
            if (at == 0 && header[0] == 0x1F && header[1] == 0x8B)
                kprintf("INITRAMFS: the archive is gzip-compressed, which is not read here; "
                        "give the bootloader an uncompressed newc cpio\n");
            else
                kprintf("INITRAMFS: no cpio header at %u, the rest is ignored\n", (unsigned)at);
            break;
        }
        uint32_t mode, file_size, name_size;
        if (hex_field(header + 14, &mode) != 0 || hex_field(header + 54, &file_size) != 0 ||
            hex_field(header + 94, &name_size) != 0 || name_size == 0) {
            kprintf("INITRAMFS: a corrupt cpio header at %u\n", (unsigned)at);
            break;
        }
        uint64_t name_at = at + CPIO_HEADER_BYTES;
        uint64_t data_at = align4(name_at + name_size);
        uint64_t next = align4(data_at + file_size);
        if (data_at > size || file_size > size - data_at) {
            kprintf("INITRAMFS: the archive ends inside a file\n");
            break;
        }
        const char *name = (const char *)archive + name_at;
        if (name[name_size - 1U] != '\0') break;
        if (strcmp(name, "TRAILER!!!") == 0) {
            at = next;
            continue;
        }
        if ((mode & 0170000U) == 0100000U && ends_with(name, ".ko") && found < capacity) {
            out[found].name = name;
            out[found].data = archive + data_at;
            out[found].size = file_size;
            found++;
        }
        at = next;
    }
    return found;
}

// fnmatch for modaliases: '*' any run, '?' one character.
static int glob_match(const char *pattern, const char *text) {
    const char *star = NULL, *resume = NULL;
    while (*text) {
        if (*pattern == '*') {
            star = pattern++;
            resume = text;
        } else if (*pattern == '?' || *pattern == *text) {
            pattern++;
            text++;
        } else if (star) {
            pattern = star + 1;
            text = ++resume;
        } else {
            return 0;
        }
    }
    while (*pattern == '*') pattern++;
    return *pattern == '\0';
}

// NAME in the comma-separated list KEY= on the command line; '-' and '_' are
// one, as modprobe has them.
static int named_on_command_line(const char *key, const char *name) {
    const char *list = boot_command_line_value(key);
    while (list && *list && *list != ' ') {
        size_t index = 0;
        for (;; index++) {
            char a = list[index], b = name[index];
            if (a == '-') a = '_';
            if (b == '-') b = '_';
            if (b == '\0') {
                if (a == '\0' || a == ',' || a == ' ') return 1;
                break;
            }
            if (a != b) break;
        }
        while (*list && *list != ',' && *list != ' ') list++;
        if (*list == ',') list++;
    }
    return 0;
}

#if defined(__x86_64__)
// The modalias of every PCI function, read from configuration space, which
// port I/O reaches before the buses are probed.
static unsigned pci_modaliases(char (*out)[MODALIAS_BYTES], unsigned capacity) {
    unsigned found = 0;
    for (unsigned bus = 0; bus < 256U; bus++) {
        for (uint8_t slot = 0; slot < 32U; slot++) {
            uint32_t header = pci_config_read32((uint8_t)bus, slot, 0, 0x0CU);
            uint8_t functions = (header & 0x00800000U) ? 8U : 1U;
            for (uint8_t function = 0; function < functions; function++) {
                uint32_t id = pci_config_read32((uint8_t)bus, slot, function, 0);
                if ((id & 0xFFFFU) == 0xFFFFU || (id & 0xFFFFU) == 0) continue;
                uint32_t class_code = pci_config_read32((uint8_t)bus, slot, function, 0x08U);
                struct pci_device device;
                memset(&device, 0, sizeof(device));
                device.bus = (uint8_t)bus;
                device.slot = slot;
                device.function = function;
                device.vendor_id = (uint16_t)id;
                device.device_id = (uint16_t)(id >> 16);
                device.class_code = (uint8_t)(class_code >> 24);
                device.subclass = (uint8_t)(class_code >> 16);
                device.prog_if = (uint8_t)(class_code >> 8);
                if (found < capacity) pci_modalias(&device, out[found++], MODALIAS_BYTES);
            }
        }
    }
    return found;
}
#else
// shortcut: on aarch64 PCI's configuration space is mapped only once the
// firmware tables are read, after this; rd.driver.pre= names what to load.
static unsigned pci_modaliases(char (*out)[MODALIAS_BYTES], unsigned capacity) {
    (void)out;
    (void)capacity;
    return 0;
}
#endif

static int wanted(const struct archive_file *file, char (*devices)[MODALIAS_BYTES],
                  unsigned device_count, char *name, size_t name_bytes) {
    if (module_image_info(file->data, file->size, "name", 0, name, name_bytes) != 0) return 0;
    if (named_on_command_line("rd.driver.blacklist", name) ||
        named_on_command_line("modprobe.blacklist", name))
        return 0;
    if (named_on_command_line("rd.driver.pre", name)) return 1;
    char alias[ALIAS_BYTES];
    for (unsigned occurrence = 0;
         module_image_info(file->data, file->size, "alias", occurrence, alias, sizeof(alias)) == 0;
         occurrence++) {
        for (unsigned device = 0; device < device_count; device++)
            if (glob_match(alias, devices[device])) return 1;
    }
    return 0;
}

int initramfs_load_modules(void) {
    const struct boot_info *boot = boot_info();
    if (!boot->initramfs_size) return 0;
    const uint8_t *archive = vmm_phys_to_virt(boot->initramfs_physical);
    static struct archive_file files[MAX_MODULES];
    static char devices[MAX_DEVICES][MODALIAS_BYTES];
    unsigned file_count = list_modules(archive, boot->initramfs_size, files, MAX_MODULES);
    unsigned device_count = pci_modaliases(devices, MAX_DEVICES);
    kprintf("INITRAMFS: %u KiB, %u modules, %u PCI functions\n",
            (unsigned)(boot->initramfs_size >> 10), file_count, device_count);

    // The modules to load, copied out of the archive, whose files are only
    // 4-byte aligned, to memory an ELF image may be read from as it likes.
    static void *images[MAX_MODULES];
    static char names[MAX_MODULES][MODULE_NAME_MAX];
    unsigned pending = 0;
    for (unsigned index = 0; index < file_count; index++) {
        images[index] = NULL;
        if (!wanted(&files[index], devices, device_count, names[index], sizeof(names[index])))
            continue;
        images[index] = kmalloc(files[index].size);
        if (!images[index]) continue;
        memcpy(images[index], files[index].data, files[index].size);
        pending++;
    }

    // One may use what another exports: each round loads what it can, until a
    // round loads nothing more; what is left then failed for good.
    unsigned loaded = 0;
    int status[MAX_MODULES];
    for (int progress = 1; pending && progress;) {
        progress = 0;
        for (unsigned index = 0; index < file_count; index++) {
            if (!images[index]) continue;
            status[index] = module_load(images[index], files[index].size, NULL);
            if (status[index] != 0 && status[index] != -EEXIST) continue;
            loaded++;
            kfree(images[index]);
            images[index] = NULL;
            pending--;
            progress = 1;
        }
    }
    for (unsigned index = 0; index < file_count; index++) {
        if (!images[index]) continue;
        kprintf("INITRAMFS: %s did not load: %d\n", names[index], status[index]);
        kfree(images[index]);
    }
    // shortcut: the archive's memory is the bootloader's and stays taken, a
    // few hundred KiB; free it if archives grow.
    return (int)loaded;
}
