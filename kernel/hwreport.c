#include <stddef.h>
#include <stdint.h>

#include "include/acpi.h"
#include "include/apic.h"
#include "include/boot.h"
#include "include/cpu.h"
#include "include/heap.h"
#include "include/hwreport.h"
#include "include/kstring.h"
#include "include/module.h"
#include "include/pci.h"
#include "include/uts.h"
#include "include/percpu.h"
#include "include/pmm.h"
#include "include/vmm.h"
#include "include/smp.h"
#include "include/time.h"
#include "include/vfs.h"

extern void kprintf(const char *fmt, ...);

#define REPORT_PATH "/tunix-hwreport.txt"
#define REPORT_MAX 32768

static char report[REPORT_MAX];
static size_t used;

static void put(const char *text) {
    if (!text) return;
    while (*text && used + 1 < sizeof(report)) report[used++] = *text++;
    report[used] = '\0';
}

static void put_number(uint64_t value) {
    char digits[24];
    int count = 0;
    if (!value) digits[count++] = '0';
    while (value && count < (int)sizeof(digits)) {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (count-- > 0 && used + 1 < sizeof(report)) report[used++] = digits[count];
    report[used] = '\0';
}

static void put_hex(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    char out[17];
    int count = 0;
    if (!value) out[count++] = '0';
    while (value && count < 16) {
        out[count++] = digits[value & 0xFU];
        value >>= 4;
    }
    put("0x");
    while (count-- > 0 && used + 1 < sizeof(report)) report[used++] = out[count];
    report[used] = '\0';
}

static void put_hex_fixed(uint64_t value, unsigned digits) {
    static const char alphabet[] = "0123456789abcdef";
    while (digits--) {
        if (used + 1 >= sizeof(report)) return;
        report[used++] = alphabet[(value >> (digits * 4)) & 0xFULL];
    }
    report[used] = '\0';
}

static void put_processor_name(void) {
    struct cpu_identity identity;
    cpu_identify(&identity);
    put(identity.model[0] ? identity.model : "unknown");
}

static void put_processor_identity(void) {
    struct cpu_identity identity;
    cpu_identify(&identity);
    put("  vendor      "); put(identity.vendor);
    put(" family "); put_number(identity.family);
    put(" model "); put_number(identity.model_number);
    put(" stepping "); put_number(identity.stepping);
    put("\n");
}

static void put_command_line(void) {
    const struct boot_info *boot = boot_info();
    put("  cmdline     ");
    put(boot && boot->command_line ? boot->command_line : "(none)");
    put("\n");
}

static void put_clock(void) {
    put("clock\n");
    put("  tsc_hz      "); put_number(time_tsc_frequency()); put("\n");
    put("  invariant   ");
    put(time_tsc_is_invariant() ? "yes" : "NO -- the counter may stop or change rate");
    put("\n");
}

static void put_processors(void) {
    const struct acpi_machine *machine = acpi_describe_machine();
    put("processors\n");
    put("  firmware    ");
    put_number(machine ? machine->cpu_count : 0);
    put(" described, ");
    put_number(smp_cpu_count());
    put(" running\n");

    if (machine) {
        for (uint32_t index = 0; index < machine->cpu_count; index++) {
            put("  madt[");
            put_number(index);
            put("]     acpi_id ");
            put_number(machine->cpus[index].acpi_id);
            put(" apic_id ");
            put_number(machine->cpus[index].apic_id);
            put(machine->cpus[index].usable ? " usable" : " NOT usable");
            put("\n");
        }
    }

    for (unsigned index = 0; index < SMP_MAX_CPUS; index++) {
        struct cpu *cpu = percpu_slot(index);
        if (!cpu || !cpu->online) continue;
        uint64_t measured = 0;
        uint32_t count = 0;
#if defined(__x86_64__)
        apic_timer_calibration(index, &measured, &count);
#endif
        put("  cpu ");
        put_number(index);
        put("       apic_id ");
        put_number(cpu->apic_id);
        if (!count) {
            put(" timer PIT");
        } else {
            put(" lapic_hz ");
            put_number(measured);
            put(" count ");
            put_number(count);
            if (count <= 1U) put("  <-- CALIBRATION FAILED, this processor will flood");
        }
        put(" clock_skew_ns ");
        put_number(time_processor_skew(index));
        put("\n");
    }
}

static void put_memory(void) {
    put("memory\n");
    put("  usable_mib  ");
    put_number(pmm_usable_page_count() * PMM_PAGE_SIZE / (1024ULL * 1024ULL));
    put("\n  free_mib    ");
    put_number(pmm_free_page_count() * PMM_PAGE_SIZE / (1024ULL * 1024ULL));
    put("\n  direct_map  ");
    put_hex(pmm_managed_limit());
    put("\n");
}


#define MODULE_DIRECTORY "/usr/lib/modules/" UTS_RELEASE "/kernel"
#define ALIAS_MAX 64

struct alias_entry {
    char module[MODULE_NAME_MAX];
    char pattern[96];
};

struct alias_table {
    struct alias_entry entry[ALIAS_MAX];
    unsigned count;
};

static int glob_match(const char *pattern, const char *text) {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            for (const char *at = text; ; at++) {
                if (glob_match(pattern, at)) return 1;
                if (!*at) return 0;
            }
        }
        if (!*text) return 0;
        if (*pattern != '?' && *pattern != *text) return 0;
        pattern++;
        text++;
    }
    return *text == '\0';
}

static void *read_file(const char *path, uint64_t *bytes) {
    struct vfs_node *node = vfs_lookup(path);
    if (!node || (node->flags & 0xFFU) != VFS_FILE || !node->length) return NULL;
    void *contents = kmalloc((size_t)node->length);
    if (!contents) return NULL;
    if (vfs_read(node, 0, (size_t)node->length, contents) != (int64_t)node->length) {
        kfree(contents);
        return NULL;
    }
    *bytes = node->length;
    return contents;
}

static void module_path(char *out, size_t capacity, const char *name) {
    size_t used = 0;
    const char *directory = MODULE_DIRECTORY "/";
    while (*directory && used + 1 < capacity) out[used++] = *directory++;
    while (*name && used + 1 < capacity) out[used++] = *name++;
    out[used] = '\0';
}

static void put_module_files(struct alias_table *aliases) {
    struct vfs_node *directory = vfs_lookup(MODULE_DIRECTORY);
    if (!directory || (directory->flags & 0xFFU) != VFS_DIRECTORY) {
        put("  installed   nothing at " MODULE_DIRECTORY "\n");
        return;
    }

    struct dirent entry;
    for (uint64_t index = 0; vfs_readdir(directory, index, &entry) == 1; index++) {
        size_t length = strlen(entry.name);
        if (length < 4 || strcmp(entry.name + length - 3, ".ko") != 0) continue;

        char path[160];
        module_path(path, sizeof(path), entry.name);
        uint64_t bytes = 0;
        void *contents = read_file(path, &bytes);
        put("  file        ");
        put(entry.name);
        if (!contents) {
            put("  UNREADABLE\n");
            continue;
        }
        put(" ");
        put_number(bytes);
        put(" bytes");

        char value[96];
        if (module_image_info(contents, (size_t)bytes, "vermagic", 0, value,
                              sizeof(value)) == 0 &&
            strcmp(value, MODULE_VERMAGIC) != 0) {
            put("  BUILT FOR \"");
            put(value);
            put("\"");
        }
        char name[MODULE_NAME_MAX];
        if (module_image_info(contents, (size_t)bytes, "name", 0, name,
                              sizeof(name)) != 0)
            name[0] = '\0';
        for (unsigned occurrence = 0; occurrence < 8U; occurrence++) {
            if (module_image_info(contents, (size_t)bytes, "alias", occurrence, value,
                                  sizeof(value)) != 0)
                break;
            put(occurrence ? ", " : ", alias ");
            put(value);
            if (aliases->count >= ALIAS_MAX || !name[0]) continue;
            struct alias_entry *slot = &aliases->entry[aliases->count++];
            strncpy(slot->module, name, sizeof(slot->module) - 1);
            slot->module[sizeof(slot->module) - 1] = '\0';
            strncpy(slot->pattern, value, sizeof(slot->pattern) - 1);
            slot->pattern[sizeof(slot->pattern) - 1] = '\0';
        }
        put("\n");
        kfree(contents);
    }
}

static void put_module_selftest(void) {
    char path[160];
    module_path(path, sizeof(path), "tunix_selftest.ko");
    uint64_t bytes = 0;
    void *contents = read_file(path, &bytes);
    put("  selftest    ");
    if (!contents) {
        put("tunix_selftest.ko is not installed, so the loader was not exercised\n");
        return;
    }

    int status = module_load(contents, (size_t)bytes, "");
    kfree(contents);
    if (status != 0) {
        put("FAILED to load, error ");
        put_number((uint64_t)(-status));
        put("\n");
        return;
    }

    struct module *module = module_find("tunix_selftest");
    uint64_t answer = 0;
    uint64_t entry = 0;
    if (!module || module_export_value(module, "tunix_selftest_answer", &entry) != 0 ||
        module_export_value(module, "tunix_selftest_run", &answer) != 0) {
        put("loaded but exports nothing the kernel can call\n");
        (void)module_unload("tunix_selftest", 0);
        return;
    }

    uint64_t expected = *(const uint64_t *)(uintptr_t)entry;
    uint64_t computed = ((uint64_t (*)(uint64_t))(uintptr_t)answer)(7);
    put("loaded at ");
    put_hex(module->base);
    put(", ");
    put_number(module->bytes);
    put(" bytes, answer ");
    put_hex(computed);
    put(computed == expected ? " PASS" : " MISMATCH");
    int removed = module_unload("tunix_selftest", 0);
    put(removed == 0 ? ", unloaded\n" : ", COULD NOT UNLOAD\n");
}

static void put_modules(struct alias_table *aliases) {
    put("modules\n");
    put("  vermagic    "); put(MODULE_VERMAGIC); put("\n");
    put("  window      "); put_hex(MODULE_VIRTUAL_BASE);
    put(" + "); put_number(MODULE_VIRTUAL_BYTES / (1024ULL * 1024ULL)); put(" MiB\n");
    put("  symbols     "); put_number(module_kernel_symbol_count());
    put(" exported to modules\n");
    put_module_files(aliases);
    put_module_selftest();

    unsigned loaded = 0;
    for (struct module *module = module_list(); module; module = module->next) {
        put("  loaded      ");
        put(module->name);
        put(" at ");
        put_hex(module->base);
        put("\n");
        loaded++;
    }
    if (!loaded) put("  loaded      none yet; udev loads them once userspace runs\n");
}

static void put_pci_device(const struct pci_device *device, void *context) {
    struct alias_table *aliases = (struct alias_table *)context;
    char alias[96];
    pci_modalias(device, alias, sizeof(alias));

    put("  0000:");
    put_hex_fixed(device->bus, 2); put(":");
    put_hex_fixed(device->slot, 2); put(".");
    put_hex_fixed(device->function, 1);
    put("  ");
    put_hex_fixed(device->vendor_id, 4); put(":"); put_hex_fixed(device->device_id, 4);
    put(" class "); put_hex_fixed(((uint32_t)device->class_code << 16) |
                                  ((uint32_t)device->subclass << 8) | device->prog_if, 6);
    put(" irq "); put_number(device->irq_line);
    put("\n    modalias  "); put(alias); put("\n");

    const char *driver = pci_device_driver(device);
    if (driver) {
        put("    driver    "); put(driver); put(" (bound)\n");
    }
    for (unsigned index = 0; index < aliases->count; index++) {
        if (!glob_match(aliases->entry[index].pattern, alias)) continue;
        put("    module    ");
        put(aliases->entry[index].module);
        put(" matches this device\n");
    }
}

static void put_pci(struct alias_table *aliases) {
    put("pci\n");
    pci_for_each_device(put_pci_device, aliases);
}

static void write_to_disk(void) {
    struct vfs_node *node = vfs_create_file_node(REPORT_PATH, 0644);
    if (!node) {
        kprintf("HWREPORT: could not create %s\n", REPORT_PATH);
        return;
    }
    (void)vfs_truncate(node, 0);
    if (vfs_write(node, 0, used, report) != (int64_t)used)
        kprintf("HWREPORT: could not write %s\n", REPORT_PATH);
    else
        kprintf("HWREPORT: written to %s\n", REPORT_PATH);
}

void hwreport_emit(void) {
    if (!boot_command_line_flag("hwreport")) return;

    used = 0;
    report[0] = '\0';
    put("tunix hardware report\n\nprocessor\n  name        ");
    put_processor_name();
    put("\n");
    put_processor_identity();
    put_command_line();
    put_clock();
    put_processors();
    put_memory();

    struct alias_table *aliases = kmalloc(sizeof(*aliases));
    if (aliases) {
        aliases->count = 0;
        put_modules(aliases);
        put_pci(aliases);
        kfree(aliases);
    }

    kprintf("%s", report);
    write_to_disk();
}
