#include <stddef.h>
#include <stdint.h>

#include <tunix/acpi.h>
#include <tunix/apic.h>
#include <tunix/boot.h>
#include <tunix/cpu.h>
#include <tunix/framebuffer.h>
#include <tunix/heap.h>
#include <tunix/hwreport.h>
#include <tunix/kstring.h>
#include <tunix/module.h>
#include <tunix/pci.h>
#include <tunix/uts.h>
#include <tunix/percpu.h>
#include <tunix/pmm.h>
#include <tunix/vmm.h>
#include <tunix/smp.h>
#include <tunix/cpufreq.h>
#include <tunix/thermal.h>
#include <tunix/time.h>
#include <tunix/vfs.h>

extern void kprintf(const char *fmt, ...);

#define REPORT_PATH "/tunix-hwreport.txt"
#define REPORT_MAX  32768

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
    put("  vendor      ");
    put(identity.vendor);
    put(" family ");
    put_number(identity.family);
    put(" model ");
    put_number(identity.model_number);
    put(" stepping ");
    put_number(identity.stepping);
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
    put("  tsc_hz      ");
    put_number(time_tsc_frequency());
    put("\n");
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

static void put_framebuffer(void) {
    if (!framebuffer_available()) return;
    put("framebuffer\n");
    put("  geometry    ");
    put_number(framebuffer_width());
    put("x");
    put_number(framebuffer_height());
    put(" pitch ");
    put_number(framebuffer_pitch());
    put("\n  mapping     ");
    put(vmm_write_combining_available() ? "write-combining" : "uncached");
    put("\n");

    uint64_t read_rate = 0;
    uint64_t write_rate = 0;
    framebuffer_measure(16U, &read_rate, &write_rate);
    put("  read        ");
    put_number(read_rate / (1024ULL * 1024ULL));
    put(" MiB/s\n  write       ");
    put_number(write_rate / (1024ULL * 1024ULL));
    put(" MiB/s\n");
    put("  a screenful ");
    put_number((uint64_t)framebuffer_pitch() * framebuffer_height() / (1024ULL * 1024ULL));
    put(" MiB, which the console never reads back\n");
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
#define ALIAS_MAX        64

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
            for (const char *at = text;; at++) {
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
        if (module_image_info(contents, (size_t)bytes, "vermagic", 0, value, sizeof(value)) == 0 &&
            strcmp(value, MODULE_VERMAGIC) != 0) {
            put("  BUILT FOR \"");
            put(value);
            put("\"");
        }
        char name[MODULE_NAME_MAX];
        if (module_image_info(contents, (size_t)bytes, "name", 0, name, sizeof(name)) != 0)
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
    put("  vermagic    ");
    put(MODULE_VERMAGIC);
    put("\n");
    put("  window      ");
    put_hex(MODULE_VIRTUAL_BASE);
    put(" + ");
    put_number(MODULE_VIRTUAL_BYTES / (1024ULL * 1024ULL));
    put(" MiB\n");
    put("  symbols     ");
    put_number(module_kernel_symbol_count());
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
    put_hex_fixed(device->bus, 2);
    put(":");
    put_hex_fixed(device->slot, 2);
    put(".");
    put_hex_fixed(device->function, 1);
    put("  ");
    put_hex_fixed(device->vendor_id, 4);
    put(":");
    put_hex_fixed(device->device_id, 4);
    put(" class ");
    put_hex_fixed(((uint32_t)device->class_code << 16) | ((uint32_t)device->subclass << 8) |
                      device->prog_if,
                  6);
    put(" irq ");
    put_number(device->irq_line);
    put("\n    modalias  ");
    put(alias);
    put("\n");

    const char *driver = pci_device_driver(device);
    if (driver) {
        put("    driver    ");
        put(driver);
        put(" (bound)\n");
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

static void put_signed(int64_t value) {
    if (value < 0) {
        put("-");
        put_number((uint64_t)-value);
    } else {
        put_number((uint64_t)value);
    }
}

static void put_acpi(void) {
    put("acpi\n");
    const struct acpi_power *power = acpi_power_info();
    if (!power) {
        put("  fadt        none\n");
        return;
    }
    const struct acpi_events *events = acpi_event_state();
    put("  sci         ");
    put_number(power->sci_interrupt);
    put(", enabled at boot ");
    put(events->sci_enabled_at_boot ? "yes" : "no");
    put(", handed over ");
    put(events->handed_over ? "yes" : "no");
    put("\n");
    put("  power key   ");
    put(events->decision ? events->decision : "not set up");
    put("\n");
    put("  firmware    embedded controller ");
    put(power->embedded_controller ? "yes" : "no");
    put(", thermal zones ");
    put_number(power->thermal_zones);
    put("\n");
    put("  gpe0        ");
    put_hex(power->gpe0_block);
    put(" length ");
    put_number(power->gpe0_length);
    put(", enabled at boot ");
    put_hex(events->gpe_enabled_at_boot);
    put("\n");
    put("  gpe1        ");
    put_hex(power->gpe1_block);
    put(" length ");
    put_number(power->gpe1_length);
    put("\n");
    put("  events      sci ");
    put_number(events->sci_count);
    put(", gpe ");
    put_number(events->gpe_events);
    put(", button ");
    put_number(events->button_events);
    put("\n");
}

static void put_thermal(void) {
    put("thermal\n");
    if (thermal_supported() <= 0) {
        put("  sensor      none (no digital thermal sensor)\n");
        return;
    }
    const struct thermal_state *state = thermal_state();
    put("  limit       ");
    put_signed(state->tjmax);
    put(" C\n");
    put("  automatic   ");
    put(state->automatic_control < 0 ? "not read yet" : state->automatic_control ? "on" : "OFF");
    put(", clock modulation ");
    put(state->clock_modulation ? "available" : "absent");
    put("\n");
    unsigned cpus = percpu_online_count();
    for (unsigned cpu = 0; cpu < cpus; cpu++) {
        struct thermal_reading reading;
        put("  cpu ");
        put_number(cpu);
        put("       ");
        if (thermal_read(cpu, &reading) != 0) {
            put("not sampled yet\n");
            continue;
        }
        put_signed(reading.celsius);
        put(" C, peak ");
        put_signed(reading.peak);
        put(" C");
        if (reading.throttled) put(", throttled");
        put("\n");
    }
}

static void put_frequency(void) {
    put("frequency\n");
    if (cpufreq_supported() <= 0) {
        put("  control     none (no enhanced speedstep this kernel drives)\n");
        return;
    }
    const struct cpufreq_state *state = cpufreq_state();
    put("  range       ");
    put_number(state->ratio_khz * state->min_ratio / 1000U);
    put(" - ");
    put_number(state->ratio_khz * state->max_ratio / 1000U);
    put(" MHz, ratios ");
    put_number(state->min_ratio);
    put(" - ");
    put_number(state->max_ratio);
    put("\n");
    put("  speedstep   ");
    put(state->eist_enabled ? "enabled" : "DISABLED by the firmware");
    put(", turbo ");
    put(state->turbo ? "available" : "absent");
    put(", target ratio ");
    put_number(state->target_ratio);
    put(state->requested ? ", raised from the firmware's" : ", kept the firmware's");
    put("\n");
    if (state->smi_counted) {
        put("  smi         ");
        put_number(state->smi_count);
        put(" system management interrupts since power-on\n");
    }
    unsigned cpus = percpu_online_count();
    for (unsigned cpu = 0; cpu < cpus; cpu++) {
        struct cpufreq_reading reading;
        put("  cpu ");
        put_number(cpu);
        put("       ");
        if (cpufreq_read(cpu, &reading) != 0) {
            put("not sampled yet\n");
            continue;
        }
        put("boot ratio ");
        put_number(reading.boot_ratio);
        put(", now ");
        put_number(reading.ratio);
        if (reading.effective_khz) {
            put(", running at ");
            put_number(reading.effective_khz / 1000U);
            put(" MHz");
        }
        put("\n");
    }
}

static int write_file(const char *path, const void *data, size_t bytes) {
    struct vfs_node *node = vfs_create_file_node(path, 0644);
    if (!node) {
        kprintf("HWREPORT: could not create %s\n", path);
        return -1;
    }
    (void)vfs_truncate(node, 0);
    if (vfs_write(node, 0, bytes, data) != (int64_t)bytes || vfs_fsync(node) != 0) {
        kprintf("HWREPORT: could not write %s\n", path);
        return -1;
    }
    kprintf("HWREPORT: written to %s\n", path);
    return 0;
}

static void write_to_disk(void) { (void)write_file(REPORT_PATH, report, used); }

#define PCI_VENDOR_NVIDIA   0x10DEU
#define PCI_CLASS_DISPLAY   0x03U
#define PCI_ROM_REGISTER    0x30U
#define PCI_ROM_ENABLE      0x1U
#define VBIOS_MAX           0x20000U
#define LEGACY_VBIOS_BASE   0xC0000ULL
#define NV_BAR0_BYTES       0x1000000ULL
#define NV_PMC_BOOT_0       0x000000U
#define NV_PBUS_PRAMIN_BASE 0x001700U
#define NV_PFB_VRAM_SIZE    0x10020CU
#define NV_PDISP_VBIOS_PTR  0x619F04U
#define NV_PRAMIN_WINDOW    0x700000U
#define GPU_REGS_PATH       "/tunix-gpu-regs.bin"

struct register_range {
    uint32_t start;
    uint32_t bytes;
};

static const struct register_range nv_register_ranges[] = {
    {0x000000U, 0x400U},  {0x001000U, 0x1000U}, {0x004000U, 0x1000U},
    {0x00E000U, 0x1000U}, {0x100000U, 0x1000U}, {0x610000U, 0x10000U},
};

struct display_list {
    struct pci_device devices[8];
    unsigned count;
};

static void collect_display(const struct pci_device *device, void *context) {
    struct display_list *list = (struct display_list *)context;
    if (device->class_code != PCI_CLASS_DISPLAY || list->count >= 8U) return;
    list->devices[list->count++] = *device;
}

static uint32_t rom_image_bytes(const uint8_t *rom, uint32_t available, uint16_t *vendor,
                                uint16_t *device, int *checksum_ok) {
    uint32_t total = 0;
    *checksum_ok = 0;
    *vendor = 0;
    *device = 0;
    for (;;) {
        if (total + 0x1AU > available) break;
        const uint8_t *image = rom + total;
        if (image[0] != 0x55U || image[1] != 0xAAU) break;
        uint16_t data = (uint16_t)(image[0x18] | (image[0x19] << 8));
        if (total + data + 0x18U > available) break;
        const uint8_t *pcir = image + data;
        if (pcir[0] != 'P' || pcir[1] != 'C' || pcir[2] != 'I' || pcir[3] != 'R') break;
        uint32_t length = (uint32_t)(pcir[0x10] | (pcir[0x11] << 8)) * 512U;
        if (!length || total + length > available) break;
        if (!total) {
            *vendor = (uint16_t)(pcir[4] | (pcir[5] << 8));
            *device = (uint16_t)(pcir[6] | (pcir[7] << 8));
            uint8_t sum = 0;
            uint32_t first = (uint32_t)image[2] * 512U;
            if (first && first <= available) {
                for (uint32_t index = 0; index < first; index++)
                    sum = (uint8_t)(sum + image[index]);
                *checksum_ok = sum == 0;
            }
        }
        total += length;
        if (pcir[0x15] & 0x80U) break;
    }
    return total;
}

static void put_vbios(const char *source, const char *path, const uint8_t *rom,
                      uint32_t available) {
    put("  vbios       ");
    put(source);
    if (!rom) {
        put(": unavailable\n");
        return;
    }
    uint16_t vendor = 0;
    uint16_t device = 0;
    int checksum_ok = 0;
    uint32_t bytes = rom_image_bytes(rom, available, &vendor, &device, &checksum_ok);
    if (!bytes) {
        put(": no 55aa image\n");
        return;
    }
    put(": ");
    put_number(bytes);
    put(" bytes, pcir ");
    put_hex_fixed(vendor, 4);
    put(":");
    put_hex_fixed(device, 4);
    put(checksum_ok ? ", checksum ok" : ", checksum BAD");
    if (write_file(path, rom, bytes) == 0) {
        put(" -> ");
        put(path);
    }
    put("\n");
}

static void copy_from_device(uint8_t *out, uint64_t mapped, uint32_t bytes) {
    volatile const uint32_t *source = (volatile const uint32_t *)mapped;
    for (uint32_t index = 0; index < bytes / 4U; index++) {
        uint32_t value = source[index];
        memcpy(out + index * 4U, &value, sizeof(value));
    }
}

static void put_legacy_vbios(uint8_t *buffer) {
#if !defined(__x86_64__)
    put_vbios("legacy 0xc0000", "/tunix-vbios-legacy.rom", NULL, 0);
    return;
#endif
    uint64_t mapped = vmm_map_device(LEGACY_VBIOS_BASE, VBIOS_MAX);
    if (mapped) copy_from_device(buffer, mapped, VBIOS_MAX);
    put_vbios("legacy 0xc0000", "/tunix-vbios-legacy.rom", mapped ? buffer : NULL, VBIOS_MAX);
}

static void put_pci_rom(const struct pci_device *device, uint8_t *buffer) {
    uint32_t saved =
        pci_config_read32(device->bus, device->slot, device->function, PCI_ROM_REGISTER);
    uint32_t address = saved & 0xFFFFF800U;
    uint32_t unused = ~address & 0xFFFFF800U;
    int sizing_mask = address >= 0xFF000000U && ((unused + 0x800U) & unused) == 0;
    put("  rom bar     ");
    put_hex(saved);
    put(sizing_mask ? " (never assigned)\n" : "\n");
    if (!address || sizing_mask) {
        put_vbios("pci rom bar", "/tunix-vbios-pcirom.rom", NULL, 0);
        return;
    }
    uint64_t mapped = vmm_map_device(address, VBIOS_MAX);
    if (mapped) {
        pci_config_write32(device->bus, device->slot, device->function, PCI_ROM_REGISTER,
                           saved | PCI_ROM_ENABLE);
        copy_from_device(buffer, mapped, VBIOS_MAX);
        pci_config_write32(device->bus, device->slot, device->function, PCI_ROM_REGISTER, saved);
    }
    put_vbios("pci rom bar", "/tunix-vbios-pcirom.rom", mapped ? buffer : NULL, VBIOS_MAX);
}

static uint32_t nv_read(uint64_t bar0, uint32_t offset) {
    return *(volatile const uint32_t *)(bar0 + offset);
}

static void nv_write(uint64_t bar0, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(bar0 + offset) = value;
}

static void put_nv_pramin(uint64_t bar0, uint8_t *buffer) {
    uint32_t pointer = nv_read(bar0, NV_PDISP_VBIOS_PTR);
    put("  vbios ptr   ");
    put_hex(pointer);
    put("\n");
    if (!(pointer & 0x8U) || (pointer & 0x3U) != 1U) {
        put_vbios("pramin", "/tunix-vbios-pramin.rom", NULL, 0);
        return;
    }
    uint64_t address = (uint64_t)(pointer & 0xFFFFFF00U) << 8;
    uint32_t saved = nv_read(bar0, NV_PBUS_PRAMIN_BASE);
    if (!address) address = ((uint64_t)saved << 16) + 0xF0000ULL;
    nv_write(bar0, NV_PBUS_PRAMIN_BASE, (uint32_t)(address >> 16));
    uint32_t inside = (uint32_t)(address & 0xFFFFULL);
    copy_from_device(buffer, bar0 + NV_PRAMIN_WINDOW + inside, VBIOS_MAX);
    nv_write(bar0, NV_PBUS_PRAMIN_BASE, saved);
    put_vbios("pramin", "/tunix-vbios-pramin.rom", buffer, VBIOS_MAX);
}

static void put_nv_registers(uint64_t bar0) {
    uint64_t bytes = 0;
    for (size_t index = 0; index < sizeof(nv_register_ranges) / sizeof(nv_register_ranges[0]);
         index++)
        bytes += 8U + nv_register_ranges[index].bytes;
    uint8_t *dump = kmalloc((size_t)bytes);
    if (!dump) return;
    uint64_t at = 0;
    for (size_t index = 0; index < sizeof(nv_register_ranges) / sizeof(nv_register_ranges[0]);
         index++) {
        const struct register_range *range = &nv_register_ranges[index];
        uint32_t header[2] = {range->start, range->bytes};
        memcpy(dump + at, header, sizeof(header));
        at += sizeof(header);
        for (uint32_t offset = 0; offset < range->bytes; offset += 4U) {
            uint32_t value = nv_read(bar0, range->start + offset);
            memcpy(dump + at, &value, sizeof(value));
            at += sizeof(value);
        }
    }
    if (write_file(GPU_REGS_PATH, dump, (size_t)at) == 0) {
        put("  registers   ");
        put_number(at);
        put(" bytes -> " GPU_REGS_PATH "\n");
    }
    kfree(dump);
}

static void put_nvidia(const struct pci_device *device, uint8_t *buffer) {
    uint64_t bar0_physical = pci_bar_address(device, 0);
    put("  bar0        ");
    put_hex(bar0_physical);
    put("\n  bar1        ");
    put_hex(pci_bar_address(device, 1));
    put("\n");
    if (!bar0_physical) return;
    uint64_t bar0 = vmm_map_device(bar0_physical, NV_BAR0_BYTES);
    if (!bar0) return;
    uint32_t boot0 = nv_read(bar0, NV_PMC_BOOT_0);
    put("  boot0       ");
    put_hex(boot0);
    put(" chipset ");
    put_hex((boot0 >> 20) & 0x1FFU);
    put(" stepping ");
    put_hex(boot0 & 0xFFU);
    put("\n");
    uint32_t vram = nv_read(bar0, NV_PFB_VRAM_SIZE);
    uint64_t vram_bytes = ((uint64_t)(vram & 0xFFU) << 32) | (vram & 0xFFFFFF00U);
    put("  vram        ");
    put_number(vram_bytes / (1024ULL * 1024ULL));
    put(" MiB\n");
    put_nv_pramin(bar0, buffer);
    put_nv_registers(bar0);
}

static void put_gpu(void) {
    put("gpu\n");
    struct display_list *list = kmalloc(sizeof(*list));
    uint8_t *buffer = kmalloc(VBIOS_MAX);
    if (!list || !buffer) {
        put("  out of memory\n");
        kfree(list);
        kfree(buffer);
        return;
    }
    list->count = 0;
    pci_for_each_device(collect_display, list);
    if (!list->count) put("  no display controller on pci\n");
    put_legacy_vbios(buffer);
    for (unsigned index = 0; index < list->count; index++) {
        const struct pci_device *device = &list->devices[index];
        put("  device      0000:");
        put_hex_fixed(device->bus, 2);
        put(":");
        put_hex_fixed(device->slot, 2);
        put(".");
        put_hex_fixed(device->function, 1);
        put(" ");
        put_hex_fixed(device->vendor_id, 4);
        put(":");
        put_hex_fixed(device->device_id, 4);
        put(" subclass ");
        put_hex_fixed(device->subclass, 2);
        put("\n");
        put_pci_rom(device, buffer);
        if (device->vendor_id == PCI_VENDOR_NVIDIA) put_nvidia(device, buffer);
    }
    kfree(buffer);
    kfree(list);
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
    put_framebuffer();
    put_memory();
    put_acpi();
    put_thermal();
    put_frequency();

    struct alias_table *aliases = kmalloc(sizeof(*aliases));
    if (aliases) {
        aliases->count = 0;
        put_modules(aliases);
        put_pci(aliases);
        kfree(aliases);
    }
    put_gpu();

    uint64_t console_started = time_uptime_ns();
    kprintf("%s", report);
    kprintf("HWREPORT: the console took %u ms to print it\n",
            (unsigned)((time_uptime_ns() - console_started) / 1000000ULL));
    write_to_disk();
}
