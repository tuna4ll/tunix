/* What this machine turned out to be, written down where somebody can read it. */
/* Everything measured here is measured on the machine that is running: the
   emulator this is developed on has a synthesised clock, one timer rate and no
   firmware quirks, so the assumptions underneath the scheduler are exactly the
   ones it cannot check. */
/* It is printed and also written to a file on the root filesystem, so a machine
   with no serial cable can still be asked what it found: boot with `hwreport`
   on the command line, then read /tunix-hwreport.txt off the disk. */

#include <stddef.h>
#include <stdint.h>

#include "include/acpi.h"
#include "include/apic.h"
#include "include/boot.h"
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

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

/* The brand string, which is the one name a person can match against a machine. */
static void put_processor_name(void) {
    uint32_t a, b, c, d;
    cpuid(0x80000000U, 0, &a, &b, &c, &d);
    if (a < 0x80000004U) { put("unknown"); return; }
    char name[49];
    for (unsigned leaf = 0; leaf < 3; leaf++) {
        cpuid(0x80000002U + leaf, 0, &a, &b, &c, &d);
        uint32_t words[4] = { a, b, c, d };
        for (unsigned word = 0; word < 4; word++)
            for (unsigned byte = 0; byte < 4; byte++)
                name[leaf * 16U + word * 4U + byte] = (char)((words[word] >> (byte * 8U)) & 0xFFU);
    }
    name[48] = '\0';
    const char *text = name;
    while (*text == ' ') text++;
    put(text);
}

static void put_processor_identity(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    char vendor[13];
    uint32_t words[3] = { b, d, c };
    for (unsigned word = 0; word < 3; word++)
        for (unsigned byte = 0; byte < 4; byte++)
            vendor[word * 4U + byte] = (char)((words[word] >> (byte * 8U)) & 0xFFU);
    vendor[12] = '\0';

    cpuid(1, 0, &a, &b, &c, &d);
    uint32_t family = (a >> 8) & 0xFU;
    uint32_t model = (a >> 4) & 0xFU;
    if (family == 0xFU) family += (a >> 20) & 0xFFU;
    if (family == 0x6U || family == 0xFU) model |= ((a >> 16) & 0xFU) << 4;

    put("  vendor      "); put(vendor);
    put(" family "); put_number(family);
    put(" model "); put_number(model);
    put(" stepping "); put_number(a & 0xFU);
    put("\n");
}

static void put_command_line(void) {
    const struct boot_info *boot = boot_info();
    put("  cmdline     ");
    put(boot && boot->command_line ? boot->command_line : "(none)");
    put("\n");
}

/* The two the scheduler rests on, and neither is checkable from an emulator. */
static void put_clock(void) {
    put("clock\n");
    put("  tsc_hz      "); put_number(time_tsc_frequency()); put("\n");
    put("  invariant   ");
    put(time_tsc_is_invariant() ? "yes" : "NO -- the counter may stop or change rate");
    put("\n");
}

/* A processor that came up, said so and then measured its own timer wrong is
   invisible from anywhere else, and a count of 1 is millions of interrupts a
   second. */
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
            /* The first processor is preempted by the PIT and never calibrates
               a local timer, so it has nothing to report here. */
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

/* Written after the console copy, so a machine that cannot finish the write has
   still said everything on the way there. */
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
