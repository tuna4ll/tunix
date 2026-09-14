#include <stddef.h>
#include <stdint.h>

#include "include/acpi.h"
#include "include/apic.h"
#include "include/boot.h"
#include "include/cpu.h"
#include "include/hwreport.h"
#include "include/kstring.h"
#include "include/percpu.h"
#include "include/pmm.h"
#include "include/smp.h"
#include "include/time.h"
#include "include/vfs.h"

extern void kprintf(const char *fmt, ...);

#define REPORT_PATH "/tunix-hwreport.txt"
#define REPORT_MAX 8192

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
        apic_timer_calibration(index, &measured, &count);
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

    kprintf("%s", report);
    write_to_disk();
}
