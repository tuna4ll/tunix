// KVM on AMD SVM: Linux's /dev/kvm ABI, so QEMU and other programs written for it run
// guests on the processor's own virtualization. Guest memory is mapped through nested
// page tables built from the memory slots userspace registers, and pinned page by page as
// the guest first touches it. The interrupt controller stays in userspace, as it does on
// Linux without KVM_CREATE_IRQCHIP: userspace injects interrupts with KVM_INTERRUPT and
// asks for an exit when the guest can take one.

#include <stddef.h>
#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/errno.h>
#include <tunix/eventfd.h>
#include <tunix/file.h>
#include <tunix/heap.h>
#include <tunix/kprintf.h>
#include <tunix/kstring.h>
#include <tunix/lock.h>
#include <tunix/module.h>
#include <tunix/mutex.h>
#include <tunix/percpu.h>
#include <tunix/pmm.h>
#include <tunix/process.h>
#include <tunix/process_arch.h>
#include <tunix/smp.h>
#include <tunix/time.h>
#include <tunix/timer.h>
#include <tunix/usercopy.h>
#include <tunix/vfs.h>
#include <tunix/vmm.h>

// ---- The ABI, as <linux/kvm.h> has it ----
//
// The numbers below were generated from the Linux header; the asserts after the
// structures check every size and offset against it.

#define KVM_GET_API_VERSION              0x0000AE00UL
#define KVM_CREATE_VM                    0x0000AE01UL
#define KVM_GET_MSR_INDEX_LIST           0xC004AE02UL
#define KVM_GET_MSR_FEATURE_INDEX_LIST   0xC004AE0AUL
#define KVM_CHECK_EXTENSION              0x0000AE03UL
#define KVM_GET_VCPU_MMAP_SIZE           0x0000AE04UL
#define KVM_GET_SUPPORTED_CPUID          0xC008AE05UL
#define KVM_GET_EMULATED_CPUID           0xC008AE09UL
#define KVM_X86_GET_MCE_CAP_SUPPORTED    0x8008AE9DUL
#define KVM_CREATE_VCPU                  0x0000AE41UL
#define KVM_SET_USER_MEMORY_REGION       0x4020AE46UL
#define KVM_GET_DIRTY_LOG                0x4010AE42UL
#define KVM_SET_TSS_ADDR                 0x0000AE47UL
#define KVM_SET_IDENTITY_MAP_ADDR        0x4008AE48UL
#define KVM_IOEVENTFD                    0x4040AE79UL
#define KVM_GET_CLOCK                    0x8030AE7CUL
#define KVM_SET_CLOCK                    0x4030AE7BUL
#define KVM_ENABLE_CAP                   0x4068AEA3UL
#define KVM_SIGNAL_MSI                   0x4020AEA5UL
#define KVM_SET_NR_MMU_PAGES             0x0000AE44UL
#define KVM_IRQ_LINE                     0x4008AE61UL
#define KVM_CREATE_IRQCHIP               0x0000AE60UL
#define KVM_RUN                          0x0000AE80UL
#define KVM_GET_REGS                     0x8090AE81UL
#define KVM_SET_REGS                     0x4090AE82UL
#define KVM_GET_SREGS                    0x8138AE83UL
#define KVM_SET_SREGS                    0x4138AE84UL
#define KVM_INTERRUPT                    0x4004AE86UL
#define KVM_NMI                          0x0000AE9AUL
#define KVM_GET_MSRS                     0xC008AE88UL
#define KVM_SET_MSRS                     0x4008AE89UL
#define KVM_SET_CPUID2                   0x4008AE90UL
#define KVM_GET_CPUID2                   0xC008AE91UL
#define KVM_GET_FPU                      0x81A0AE8CUL
#define KVM_SET_FPU                      0x41A0AE8DUL
#define KVM_GET_XSAVE                    0x9000AEA4UL
#define KVM_SET_XSAVE                    0x5000AEA5UL
#define KVM_GET_XCRS                     0x8188AEA6UL
#define KVM_SET_XCRS                     0x4188AEA7UL
#define KVM_GET_VCPU_EVENTS              0x8040AE9FUL
#define KVM_SET_VCPU_EVENTS              0x4040AEA0UL
#define KVM_GET_MP_STATE                 0x8004AE98UL
#define KVM_SET_MP_STATE                 0x4004AE99UL
#define KVM_GET_DEBUGREGS                0x8080AEA1UL
#define KVM_SET_DEBUGREGS                0x4080AEA2UL
#define KVM_X86_SETUP_MCE                0x4008AE9CUL
#define KVM_X86_SET_MCE                  0x4040AE9EUL
#define KVM_GET_TSC_KHZ                  0x0000AEA3UL
#define KVM_SET_TSC_KHZ                  0x0000AEA2UL
#define KVM_SET_SIGNAL_MASK              0x4004AE8BUL
#define KVM_KVMCLOCK_CTRL                0x0000AEADUL
#define KVM_SET_GUEST_DEBUG              0x4048AE9BUL
#define KVM_TRANSLATE                    0xC018AE85UL
#define KVM_CAP_IRQCHIP                  0
#define KVM_CAP_HLT                      1
#define KVM_CAP_USER_MEMORY              3
#define KVM_CAP_SET_TSS_ADDR             4
#define KVM_CAP_VAPIC                    6
#define KVM_CAP_EXT_CPUID                7
#define KVM_CAP_NR_VCPUS                 9
#define KVM_CAP_NR_MEMSLOTS              10
#define KVM_CAP_MP_STATE                 14
#define KVM_CAP_DESTROY_MEMORY_REGION_WORKS 21
#define KVM_CAP_JOIN_MEMORY_REGIONS_WORKS 30
#define KVM_CAP_IRQ_ROUTING              25
#define KVM_CAP_MCE                      31
#define KVM_CAP_IOEVENTFD                36
#define KVM_CAP_ADJUST_CLOCK             39
#define KVM_CAP_INTERNAL_ERROR_DATA      40
#define KVM_CAP_VCPU_EVENTS              41
#define KVM_CAP_SET_IDENTITY_MAP_ADDR    37
#define KVM_CAP_XSAVE                    55
#define KVM_CAP_DEBUGREGS                50
#define KVM_CAP_X86_ROBUST_SINGLESTEP    51
#define KVM_CAP_XCRS                     56
#define KVM_CAP_GET_TSC_KHZ              61
#define KVM_CAP_SIGNAL_MSI               77
#define KVM_CAP_IOEVENTFD_ANY_LENGTH     122
#define KVM_CAP_IMMEDIATE_EXIT           136
#define KVM_CAP_MAX_VCPUS                66
#define KVM_CAP_MAX_VCPU_ID              128
#define KVM_CAP_NOP_IO_DELAY             12
#define KVM_CAP_SYNC_MMU                 16
#define KVM_CAP_EXT_EMUL_CPUID           95
#define KVM_CAP_TSC_DEADLINE_TIMER       72
#define KVM_EXIT_UNKNOWN                 0
#define KVM_EXIT_IO                      2
#define KVM_EXIT_HLT                     5
#define KVM_EXIT_MMIO                    6
#define KVM_EXIT_IRQ_WINDOW_OPEN         7
#define KVM_EXIT_SHUTDOWN                8
#define KVM_EXIT_FAIL_ENTRY              9
#define KVM_EXIT_INTR                    10
#define KVM_EXIT_INTERNAL_ERROR          17
#define KVM_EXIT_SYSTEM_EVENT            24
#define KVM_INTERNAL_ERROR_EMULATION     1
#define KVM_INTERNAL_ERROR_SIMUL_EX      2
#define KVM_EXIT_IO_IN                   0
#define KVM_EXIT_IO_OUT                  1
#define KVM_MEM_LOG_DIRTY_PAGES          1
#define KVM_MEM_READONLY                 2
#define KVM_IOEVENTFD_FLAG_DATAMATCH     1
#define KVM_IOEVENTFD_FLAG_PIO           2
#define KVM_IOEVENTFD_FLAG_DEASSIGN      4
#define KVM_CPUID_FLAG_SIGNIFCANT_INDEX  1
#define KVM_MP_STATE_RUNNABLE            0
#define KVM_MP_STATE_UNINITIALIZED       1
#define KVM_MP_STATE_HALTED              3
#define KVM_VCPUEVENT_VALID_NMI_PENDING  1
#define KVM_VCPUEVENT_VALID_SIPI_VECTOR  2
#define KVM_VCPUEVENT_VALID_SHADOW       4

#define KVM_IRQ_LINE_STATUS              0xC008AE67UL
#define KVM_SET_GSI_ROUTING              0x4008AE6AUL
#define KVM_IRQFD                        0x4020AE76UL
#define KVM_GET_LAPIC                    0x8400AE8EUL
#define KVM_SET_LAPIC                    0x4400AE8FUL
#define KVM_GET_IRQCHIP                  0xC208AE62UL
#define KVM_SET_IRQCHIP                  0x8208AE63UL
#define KVM_CREATE_PIT2                  0x4040AE77UL
#define KVM_GET_PIT2                     0x8070AE9FUL
#define KVM_SET_PIT2                     0x4070AEA0UL
#define KVM_REINJECT_CONTROL             0x0000AE71UL
#define KVM_SET_BOOT_CPU_ID              0x0000AE78UL
#define KVM_TPR_ACCESS_REPORTING         0xC028AE92UL
#define KVM_SET_VAPIC_ADDR               0x4008AE93UL
#define KVM_GET_STATS_FD                 0x0000AECEUL
#define KVM_CAP_IRQFD                    32
#define KVM_CAP_SPLIT_IRQCHIP            121
#define KVM_CAP_PIT2                     33
#define KVM_CAP_PIT_STATE2               35
#define KVM_CAP_IRQ_INJECT_STATUS        26
#define KVM_CAP_REINJECT_CONTROL         24
#define KVM_CAP_SET_BOOT_CPU_ID          34
#define KVM_CAP_IRQFD_RESAMPLE           82
#define KVM_CAP_X2APIC_API               129
#define KVM_CAP_VAPIC                    6
#define KVM_EXIT_IOAPIC_EOI              26
#define KVM_IRQ_ROUTING_IRQCHIP          1
#define KVM_IRQ_ROUTING_MSI              2
#define KVM_IRQFD_FLAG_DEASSIGN          1
#define KVM_IRQFD_FLAG_RESAMPLE          2
#define KVM_MP_STATE_INIT_RECEIVED       2
#define KVM_MP_STATE_SIPI_RECEIVED       4
#define KVM_IRQCHIP_PIC_MASTER           0
#define KVM_IRQCHIP_PIC_SLAVE            1
#define KVM_IRQCHIP_IOAPIC               2
#define KVM_IOAPIC_NUM_PINS              24
#define KVM_PIT_SPEAKER_DUMMY            1

#define KVM_API_VERSION        12
#define KVM_RUN_PAGES          2U
#define KVM_IO_DATA_OFFSET     4096U
#define KVM_GUESTDBG_ENABLE    1U
#define KVM_CLOCK_TSC_STABLE   2U

struct kvm_regs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rsp, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags;
};

struct kvm_segment {
    uint64_t base;
    uint32_t limit;
    uint16_t selector;
    uint8_t type, present, dpl, db, s, l, g, avl, unusable, padding;
};

struct kvm_dtable {
    uint64_t base;
    uint16_t limit;
    uint16_t padding[3];
};

struct kvm_sregs {
    struct kvm_segment cs, ds, es, fs, gs, ss, tr, ldt;
    struct kvm_dtable gdt, idt;
    uint64_t cr0, cr2, cr3, cr4, cr8, efer, apic_base;
    uint64_t interrupt_bitmap[4];
};

struct kvm_fpu {
    uint8_t fpr[8][16];
    uint16_t fcw, fsw;
    uint8_t ftwx, pad1;
    uint16_t last_opcode;
    uint64_t last_ip, last_dp;
    uint8_t xmm[16][16];
    uint32_t mxcsr, pad2;
};

struct kvm_msr_entry {
    uint32_t index, reserved;
    uint64_t data;
};

struct kvm_msrs {
    uint32_t nmsrs, pad;
    struct kvm_msr_entry entries[];
};

struct kvm_msr_list {
    uint32_t nmsrs;
    uint32_t indices[];
};

struct kvm_cpuid_entry2 {
    uint32_t function, index, flags, eax, ebx, ecx, edx;
    uint32_t padding[3];
};

struct kvm_cpuid2 {
    uint32_t nent, padding;
    struct kvm_cpuid_entry2 entries[];
};

struct kvm_userspace_memory_region {
    uint32_t slot, flags;
    uint64_t guest_phys_addr, memory_size, userspace_addr;
};

struct kvm_dirty_log {
    uint32_t slot, padding1;
    uint64_t dirty_bitmap;
};

struct kvm_ioeventfd {
    uint64_t datamatch, addr;
    uint32_t len;
    int32_t fd;
    uint32_t flags;
    uint8_t pad[36];
};

struct kvm_clock_data {
    uint64_t clock;
    uint32_t flags, pad0;
    uint64_t realtime, host_tsc;
    uint32_t pad[4];
};

struct kvm_xsave {
    uint32_t region[1024];
};

struct kvm_xcr {
    uint32_t xcr, reserved;
    uint64_t value;
};

struct kvm_xcrs {
    uint32_t nr_xcrs, flags;
    struct kvm_xcr xcrs[16];
    uint64_t padding[16];
};

struct kvm_vcpu_events {
    struct {
        uint8_t injected, nr, has_error_code, pending;
        uint32_t error_code;
    } exception;
    struct {
        uint8_t injected, nr, soft, shadow;
    } interrupt;
    struct {
        uint8_t injected, pending, masked, pad;
    } nmi;
    uint32_t sipi_vector;
    uint32_t flags;
    struct {
        uint8_t smm, pending, smm_inside_nmi, latched_init;
    } smi;
    struct {
        uint8_t pending;
    } triple_fault;
    uint8_t reserved[26];
    uint8_t exception_has_payload;
    uint64_t exception_payload;
};

struct kvm_mp_state {
    uint32_t mp_state;
};

struct kvm_debugregs {
    uint64_t db[4];
    uint64_t dr6, dr7, flags;
    uint64_t reserved[9];
};

struct kvm_interrupt {
    uint32_t irq;
};

struct kvm_enable_cap {
    uint32_t cap, flags;
    uint64_t args[4];
    uint8_t pad[64];
};

struct kvm_x86_mce {
    uint64_t status, addr, misc, mcg_status;
    uint8_t bank;
    uint8_t pad1[7];
    uint64_t pad2[3];
};

struct kvm_msi {
    uint32_t address_lo, address_hi, data, flags, devid;
    uint8_t pad[12];
};

struct kvm_signal_mask {
    uint32_t len;
    uint8_t sigset[];
};

struct kvm_lapic_state {
    char regs[1024];
};

struct kvm_irq_routing_entry {
    uint32_t gsi, type, flags, pad;
    union {
        struct {
            uint32_t irqchip, pin;
        } irqchip;
        struct {
            uint32_t address_lo, address_hi, data, devid;
        } msi;
        uint32_t pad[8];
    } u;
};

struct kvm_irq_routing {
    uint32_t nr, flags;
    struct kvm_irq_routing_entry entries[];
};

struct kvm_irqfd {
    uint32_t fd, gsi, flags, resamplefd;
    uint8_t pad[16];
};

struct kvm_irq_level {
    uint32_t irq;
    int32_t level;
};

struct kvm_pic_state {
    uint8_t last_irr, irr, imr, isr, priority_add, irq_base, read_reg_select, poll;
    uint8_t special_mask, init_state, auto_eoi, rotate_on_auto_eoi;
    uint8_t special_fully_nested_mode, init4, elcr, elcr_mask;
};

struct kvm_ioapic_state {
    uint64_t base_address;
    uint32_t ioregsel, id, irr, pad;
    uint64_t redirtbl[24];
};

struct kvm_irqchip {
    uint32_t chip_id, pad;
    union {
        char dummy[512];
        struct kvm_pic_state pic;
        struct kvm_ioapic_state ioapic;
    } chip;
};

struct kvm_pit_channel_state {
    uint32_t count;
    uint16_t latched_count;
    uint8_t count_latched, status_latched, status, read_state, write_state, write_latch;
    uint8_t rw_mode, mode, bcd, gate;
    int64_t count_load_time;
};

struct kvm_pit_state2 {
    struct kvm_pit_channel_state channels[3];
    uint32_t flags;
    uint32_t reserved[9];
};

struct kvm_pit_config {
    uint32_t flags;
    uint32_t pad[15];
};

struct kvm_run {
    uint8_t request_interrupt_window;
    uint8_t immediate_exit;
    uint8_t padding1[6];
    uint32_t exit_reason;
    uint8_t ready_for_interrupt_injection;
    uint8_t if_flag;
    uint16_t flags;
    uint64_t cr8;
    uint64_t apic_base;
    union {
        struct {
            uint64_t hardware_exit_reason;
        } hw;
        struct {
            uint64_t hardware_entry_failure_reason;
            uint32_t cpu;
        } fail_entry;
        struct {
            uint8_t direction, size;
            uint16_t port;
            uint32_t count;
            uint64_t data_offset;
        } io;
        struct {
            uint64_t phys_addr;
            uint8_t data[8];
            uint32_t len;
            uint8_t is_write;
        } mmio;
        struct {
            uint32_t suberror, ndata;
            uint64_t data[16];
        } internal;
        struct {
            uint8_t vector;
        } eoi;
        char padding[256];
    };
    uint64_t kvm_valid_regs;
    uint64_t kvm_dirty_regs;
    union {
        char padding[2048];
    } s;
};

_Static_assert(sizeof(struct kvm_run) == 2352, "kvm_run");
_Static_assert(offsetof(struct kvm_run, request_interrupt_window) == 0, "kvm_run.request_interrupt_window");
_Static_assert(offsetof(struct kvm_run, immediate_exit) == 1, "kvm_run.immediate_exit");
_Static_assert(offsetof(struct kvm_run, exit_reason) == 8, "kvm_run.exit_reason");
_Static_assert(offsetof(struct kvm_run, ready_for_interrupt_injection) == 12, "kvm_run.ready_for_interrupt_injection");
_Static_assert(offsetof(struct kvm_run, if_flag) == 13, "kvm_run.if_flag");
_Static_assert(offsetof(struct kvm_run, flags) == 14, "kvm_run.flags");
_Static_assert(offsetof(struct kvm_run, cr8) == 16, "kvm_run.cr8");
_Static_assert(offsetof(struct kvm_run, apic_base) == 24, "kvm_run.apic_base");
_Static_assert(offsetof(struct kvm_run, io) == 32, "kvm_run.io");
_Static_assert(offsetof(struct kvm_run, mmio) == 32, "kvm_run.mmio");
_Static_assert(offsetof(struct kvm_run, internal) == 32, "kvm_run.internal");
_Static_assert(offsetof(struct kvm_run, kvm_valid_regs) == 288, "kvm_run.kvm_valid_regs");
_Static_assert(offsetof(struct kvm_run, s) == 304, "kvm_run.s");
_Static_assert(sizeof(struct kvm_regs) == 144, "kvm_regs");
_Static_assert(sizeof(struct kvm_segment) == 24, "kvm_segment");
_Static_assert(sizeof(struct kvm_dtable) == 16, "kvm_dtable");
_Static_assert(sizeof(struct kvm_sregs) == 312, "kvm_sregs");
_Static_assert(offsetof(struct kvm_sregs, gdt) == 192, "kvm_sregs.gdt");
_Static_assert(offsetof(struct kvm_sregs, cr0) == 224, "kvm_sregs.cr0");
_Static_assert(offsetof(struct kvm_sregs, efer) == 264, "kvm_sregs.efer");
_Static_assert(offsetof(struct kvm_sregs, interrupt_bitmap) == 280, "kvm_sregs.interrupt_bitmap");
_Static_assert(sizeof(struct kvm_fpu) == 416, "kvm_fpu");
_Static_assert(offsetof(struct kvm_fpu, xmm) == 152, "kvm_fpu.xmm");
_Static_assert(offsetof(struct kvm_fpu, mxcsr) == 408, "kvm_fpu.mxcsr");
_Static_assert(sizeof(struct kvm_msr_entry) == 16, "kvm_msr_entry");
_Static_assert(sizeof(struct kvm_msrs) == 8, "kvm_msrs");
_Static_assert(sizeof(struct kvm_msr_list) == 4, "kvm_msr_list");
_Static_assert(sizeof(struct kvm_cpuid_entry2) == 40, "kvm_cpuid_entry2");
_Static_assert(sizeof(struct kvm_cpuid2) == 8, "kvm_cpuid2");
_Static_assert(sizeof(struct kvm_userspace_memory_region) == 32, "kvm_userspace_memory_region");
_Static_assert(sizeof(struct kvm_dirty_log) == 16, "kvm_dirty_log");
_Static_assert(sizeof(struct kvm_ioeventfd) == 64, "kvm_ioeventfd");
_Static_assert(offsetof(struct kvm_ioeventfd, flags) == 24, "kvm_ioeventfd.flags");
_Static_assert(sizeof(struct kvm_clock_data) == 48, "kvm_clock_data");
_Static_assert(sizeof(struct kvm_xsave) == 4096, "kvm_xsave");
_Static_assert(sizeof(struct kvm_xcrs) == 392, "kvm_xcrs");
_Static_assert(sizeof(struct kvm_xcr) == 16, "kvm_xcr");
_Static_assert(sizeof(struct kvm_vcpu_events) == 64, "kvm_vcpu_events");
_Static_assert(offsetof(struct kvm_vcpu_events, nmi) == 12, "kvm_vcpu_events.nmi");
_Static_assert(offsetof(struct kvm_vcpu_events, sipi_vector) == 16, "kvm_vcpu_events.sipi_vector");
_Static_assert(offsetof(struct kvm_vcpu_events, flags) == 20, "kvm_vcpu_events.flags");
_Static_assert(offsetof(struct kvm_vcpu_events, smi) == 24, "kvm_vcpu_events.smi");
_Static_assert(offsetof(struct kvm_vcpu_events, exception_has_payload) == 55, "kvm_vcpu_events.exception_has_payload");
_Static_assert(sizeof(struct kvm_mp_state) == 4, "kvm_mp_state");
_Static_assert(sizeof(struct kvm_debugregs) == 128, "kvm_debugregs");
_Static_assert(sizeof(struct kvm_interrupt) == 4, "kvm_interrupt");
_Static_assert(sizeof(struct kvm_enable_cap) == 104, "kvm_enable_cap");
_Static_assert(sizeof(struct kvm_x86_mce) == 64, "kvm_x86_mce");
_Static_assert(sizeof(struct kvm_msi) == 32, "kvm_msi");
_Static_assert(sizeof(struct kvm_signal_mask) == 4, "kvm_signal_mask");
_Static_assert(sizeof(struct kvm_lapic_state) == 1024, "kvm_lapic_state");
_Static_assert(sizeof(struct kvm_irq_routing_entry) == 48, "kvm_irq_routing_entry");
_Static_assert(offsetof(struct kvm_irq_routing_entry, u) == 16, "kvm_irq_routing_entry.u");
_Static_assert(sizeof(struct kvm_irq_routing) == 8, "kvm_irq_routing");
_Static_assert(sizeof(struct kvm_irqfd) == 32, "kvm_irqfd");
_Static_assert(sizeof(struct kvm_irq_level) == 8, "kvm_irq_level");
_Static_assert(offsetof(struct kvm_run, eoi) == 32, "kvm_run.eoi");
_Static_assert(sizeof(struct kvm_pic_state) == 16, "kvm_pic_state");
_Static_assert(sizeof(struct kvm_ioapic_state) == 216, "kvm_ioapic_state");
_Static_assert(offsetof(struct kvm_ioapic_state, redirtbl) == 24, "kvm_ioapic_state.redirtbl");
_Static_assert(sizeof(struct kvm_irqchip) == 520, "kvm_irqchip");
_Static_assert(offsetof(struct kvm_irqchip, chip) == 8, "kvm_irqchip.chip");
_Static_assert(sizeof(struct kvm_pit_state2) == 112, "kvm_pit_state2");
_Static_assert(sizeof(struct kvm_pit_channel_state) == 24, "kvm_pit_channel_state");
_Static_assert(sizeof(struct kvm_pit_config) == 64, "kvm_pit_config");

// ---- SVM: the VMCB, intercepts and exit codes (AMD APM volume 2, appendix B) ----

#define MSR_EFER        0xC0000080U
#define MSR_VM_CR       0xC0010114U
#define MSR_VM_HSAVE_PA 0xC0010117U
#define EFER_SCE        (1ULL << 0)
#define EFER_LME        (1ULL << 8)
#define EFER_LMA        (1ULL << 10)
#define EFER_NXE        (1ULL << 11)
#define EFER_SVME       (1ULL << 12)
#define EFER_FFXSR      (1ULL << 14)
#define VM_CR_SVMDIS    (1ULL << 4)

#define VMCB_INTERCEPT_EXCEPTIONS 0x008
#define VMCB_INTERCEPT_MISC1      0x00C
#define VMCB_INTERCEPT_MISC2      0x010
#define VMCB_IOPM_BASE            0x040
#define VMCB_MSRPM_BASE           0x048
#define VMCB_TSC_OFFSET           0x050
#define VMCB_ASID                 0x058
#define VMCB_TLB_CONTROL          0x05C
#define VMCB_INT_CTL              0x060
#define VMCB_INT_STATE            0x068
#define VMCB_EXIT_CODE            0x070
#define VMCB_EXIT_INFO1           0x078
#define VMCB_EXIT_INFO2           0x080
#define VMCB_EXIT_INT_INFO        0x088
#define VMCB_NP_ENABLE            0x090
#define VMCB_EVENT_INJ            0x0A8
#define VMCB_NCR3                 0x0B0
#define VMCB_CLEAN                0x0C0
#define VMCB_NEXT_RIP             0x0C8

#define VMCB_ES     0x400
#define VMCB_CS     0x410
#define VMCB_SS     0x420
#define VMCB_DS     0x430
#define VMCB_FS     0x440
#define VMCB_GS     0x450
#define VMCB_GDTR   0x460
#define VMCB_LDTR   0x470
#define VMCB_IDTR   0x480
#define VMCB_TR     0x490
#define VMCB_CPL    0x4CB
#define VMCB_EFER   0x4D0
#define VMCB_CR4    0x548
#define VMCB_CR3    0x550
#define VMCB_CR0    0x558
#define VMCB_DR7    0x560
#define VMCB_DR6    0x568
#define VMCB_RFLAGS 0x570
#define VMCB_RIP    0x578
#define VMCB_RSP    0x5D8
#define VMCB_RAX    0x5F8
#define VMCB_STAR   0x600
#define VMCB_LSTAR  0x608
#define VMCB_CSTAR  0x610
#define VMCB_SFMASK 0x618
#define VMCB_KGS    0x620
#define VMCB_SYSENTER_CS  0x628
#define VMCB_SYSENTER_ESP 0x630
#define VMCB_SYSENTER_EIP 0x638
#define VMCB_CR2    0x640
#define VMCB_G_PAT  0x668

// Segment attributes as the VMCB packs them.
#define SEG_TYPE(a)    ((a) & 0xFU)
#define SEG_S          (1U << 4)
#define SEG_P          (1U << 7)
#define SEG_AVL        (1U << 8)
#define SEG_L          (1U << 9)
#define SEG_DB         (1U << 10)
#define SEG_G          (1U << 11)

#define INTERCEPT_INTR        (1U << 0)
#define INTERCEPT_NMI         (1U << 1)
#define INTERCEPT_SMI         (1U << 2)
#define INTERCEPT_INIT        (1U << 3)
#define INTERCEPT_VINTR       (1U << 4)
#define INTERCEPT_CPUID       (1U << 18)
#define INTERCEPT_IRET        (1U << 20)
#define INTERCEPT_INVD        (1U << 22)
#define INTERCEPT_HLT         (1U << 24)
#define INTERCEPT_INVLPGA     (1U << 26)
#define INTERCEPT_IOIO        (1U << 27)
#define INTERCEPT_MSR         (1U << 28)
#define INTERCEPT_TASK_SWITCH (1U << 29)
#define INTERCEPT_SHUTDOWN    (1U << 31)
#define INTERCEPT_VMRUN       (1U << 0)
#define INTERCEPT_VMMCALL     (1U << 1)
#define INTERCEPT_VMLOAD      (1U << 2)
#define INTERCEPT_VMSAVE      (1U << 3)
#define INTERCEPT_STGI        (1U << 4)
#define INTERCEPT_CLGI        (1U << 5)
#define INTERCEPT_SKINIT      (1U << 6)
#define INTERCEPT_MONITOR     (1U << 10)
#define INTERCEPT_MWAIT       (1U << 11)
#define INTERCEPT_MWAIT_COND  (1U << 12)
#define INTERCEPT_XSETBV      (1U << 13)
#define INTERCEPT_RDPRU       (1U << 14)

#define INT_CTL_V_TPR_MASK    0xFULL
#define INT_CTL_V_IRQ         (1ULL << 8)
#define INT_CTL_V_PRIO_SHIFT  16
#define INT_CTL_V_IGN_TPR     (1ULL << 20)
#define INT_CTL_V_INTR_MASK   (1ULL << 24)

#define EVENT_VALID           (1ULL << 31)
#define EVENT_ERROR_VALID     (1ULL << 11)
#define EVENT_TYPE_SHIFT      8
#define EVENT_TYPE_INTR       0ULL
#define EVENT_TYPE_NMI        2ULL
#define EVENT_TYPE_EXCEPTION  3ULL
#define EVENT_TYPE_SOFT       4ULL

#define EXIT_EXCEPTION_DB  0x41
#define EXIT_EXCEPTION_AC  0x51
#define EXIT_INTR          0x60
#define EXIT_NMI           0x61
#define EXIT_SMI           0x62
#define EXIT_INIT          0x63
#define EXIT_VINTR         0x64
#define EXIT_CPUID         0x72
#define EXIT_IRET          0x74
#define EXIT_INVD          0x76
#define EXIT_HLT           0x78
#define EXIT_INVLPGA       0x7A
#define EXIT_IOIO          0x7B
#define EXIT_MSR           0x7C
#define EXIT_TASK_SWITCH   0x7D
#define EXIT_SHUTDOWN      0x7F
#define EXIT_VMRUN         0x80
#define EXIT_VMMCALL       0x81
#define EXIT_VMLOAD        0x82
#define EXIT_VMSAVE        0x83
#define EXIT_STGI          0x84
#define EXIT_CLGI          0x85
#define EXIT_SKINIT        0x86
#define EXIT_MONITOR       0x8A
#define EXIT_MWAIT         0x8B
#define EXIT_MWAIT_COND    0x8C
#define EXIT_XSETBV        0x8D
#define EXIT_RDPRU         0x8E
#define EXIT_NPF           0x400
#define EXIT_INVALID       0xFFFFFFFFFFFFFFFFULL

#define VECTOR_DB  1U
#define VECTOR_NMI 2U
#define VECTOR_UD  6U
#define VECTOR_GP  13U
#define VECTOR_AC  17U

#define RFLAGS_IF (1ULL << 9)
#define RFLAGS_DF (1ULL << 10)
#define RFLAGS_VM (1ULL << 17)
#define CR0_PE    (1ULL << 0)
#define CR0_PG    (1ULL << 31)
#define CR4_PSE   (1ULL << 4)
#define CR4_PAE   (1ULL << 5)
#define CR4_OSXSAVE (1ULL << 18)

#define NPT_PRESENT 0x1ULL
#define NPT_LEAF    0x7ULL  /* present, writable, user */
#define NPT_ADDRESS 0x000FFFFFFFFFF000ULL

#define KVM_SLOTS        256U
#define KVM_MAX_ROUTES   1024U
#define KVM_MAX_VCPUS    64U
#define KVM_CPUID_MAX    256U
#define KVM_MCE_BANKS    32U
#define MCG_CTL_P        (1ULL << 8)
#define MCG_SER_P        (1ULL << 24)
#define FAULT_AROUND     16ULL
#define GPA_LIMIT        (1ULL << 48)

// Guest registers as the entry code keeps them, in x86 encoding order. RAX and RSP live
// in the VMCB; their slots here are unused.
enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI, R_8, R_9, R_10, R_11, R_12, R_13,
       R_14, R_15, R_COUNT };

// What the next KVM_RUN finishes before it enters the guest: the data userspace read for
// an IN or an MMIO load.
enum completion_kind { DONE_NONE, DONE_PIO_IN, DONE_INS, DONE_MMIO_REG, DONE_MMIO_MEMORY };

struct completion {
    enum completion_kind kind;
    unsigned size;
    unsigned count;
    unsigned dest_size;
    int reg;
    int high_byte;
    int sign_extend;
    uint64_t linear;
};

struct memory_slot {
    uint32_t id;
    uint32_t flags;
    uint64_t gpa;
    uint64_t size;
    uint64_t hva;
};

struct ioeventfd {
    struct ioeventfd *next;
    uint64_t address;
    uint64_t datamatch;
    uint32_t length;
    int pio;
    int match;
    struct file *file;
};

struct kvm_vm;

struct kvm_vcpu {
    struct mutex lock;
    struct kvm_vm *vm;
    uint32_t id;
    uint64_t serial;
    uint8_t *vmcb;
    uint64_t vmcb_pa;
    uint64_t run_pages[KVM_RUN_PAGES];
    struct kvm_run *run;
    uint8_t *io_data;
    uint64_t regs[R_COUNT];
    void *fpu_allocation;
    uint8_t *guest_fpu;
    uint8_t *host_fpu;
    uint64_t xcr0;
    uint64_t dr[4];
    uint64_t apic_base;
    uint64_t misc_enable;
    uint64_t hwcr;
    uint64_t mcg_cap, mcg_status, mcg_ctl;
    uint64_t mce_banks[KVM_MCE_BANKS * 4];
    uint64_t mtrr_default;
    uint64_t mtrr_variable[16];
    uint64_t mtrr_fixed[11];
    struct kvm_cpuid_entry2 *cpuid;
    uint32_t cpuid_count;
    uint64_t reinject;
    int exception_pending;
    uint8_t exception_vector;
    int exception_has_error;
    uint32_t exception_error;
    int irq_pending;
    uint8_t irq_vector;
    volatile int nmi_pending;
    int nmi_masked;
    int awaiting_iret;
    uint64_t iret_rip;
    uint32_t mp_state;
    volatile uint32_t sipi_vector;
    int flush;
    struct completion done;
    volatile int in_guest;
    volatile uint64_t entered_generation;
    // The in-kernel local APIC: its register page, as KVM_GET_LAPIC has it, and its timer.
    uint8_t *apic;
    uint64_t timer_deadline;
    uint64_t timer_period;
    uint8_t vtpr_entered;
    // What other vCPUs did to this one (INIT, SIPI), and how they reach it.
    volatile uint32_t pending;
    volatile int kick;
    volatile unsigned cpu;
    int wake;
};

struct irqfd;

#define IRQCHIP_SPLIT      1
#define IRQCHIP_FULL       2
#define IOAPIC_BASE        0xFEC00000ULL
#define IOAPIC_VERSION     0x11U
#define IOAPIC_MASKED      (1ULL << 16)
#define IOAPIC_LEVEL       (1ULL << 15)
#define IOAPIC_REMOTE_IRR  (1ULL << 14)
#define IOAPIC_DELIVERING  (1ULL << 12)
#define PIT_FREQUENCY      1193182ULL

#define pic_chip kvm_pic_state

struct ioapic_chip {
    uint32_t ioregsel, id, irr;
    uint64_t redirection[24];
};

struct pit_channel {
    uint32_t count;
    uint16_t latched_count;
    uint8_t count_latched, status_latched, status, read_state, write_state, write_latch;
    uint8_t rw_mode, mode, bcd, gate;
    uint64_t load_time;
};
_Static_assert(sizeof(struct pit_channel) == sizeof(struct kvm_pit_channel_state), "pit_channel");

struct pit_chip {
    struct pit_channel channels[3];
    uint32_t flags;
    uint8_t speaker_data;
    // When channel 0 next raises IRQ0, or 0.
    volatile uint64_t deadline;
};

struct kvm_vm {
    struct mutex lock;
    int refs;
    uint64_t cr3;
    uint64_t npt_root;
    struct memory_slot slots[KVM_SLOTS];
    uint32_t slot_count;
    struct ioeventfd *ioeventfds;
    volatile uint64_t generation;
    struct kvm_vcpu *vcpus[KVM_MAX_VCPUS];
    int64_t clock_offset;
    uint32_t vcpus_created;
    // The local APICs are in the kernel (KVM_CAP_SPLIT_IRQCHIP).
    int irqchip;
    uint32_t ioapic_pins;
    // The vCPU list as interrupt delivery walks it, the GSI routes and the irqfds.
    struct mutex irq_lock;
    struct kvm_irq_routing_entry *routes;
    uint32_t route_count;
    struct irqfd *irqfds;
    // With KVM_CREATE_IRQCHIP, the PICs and I/O APIC; with KVM_CREATE_PIT2, the PIT.
    struct pic_chip pic[2];
    struct ioapic_chip ioapic;
    struct pit_chip pit;
    int pit_created;
};

struct svm_cpu {
    uint64_t hsave;
    uint64_t host_save;
    int enabled;
    uint64_t last_vcpu;
};

static struct svm_cpu *svm_cpus;
static unsigned svm_cpu_slots;
static uint64_t iopm_pa;
static uint64_t msrpm_pa;
static uint64_t host_xcr0;
static uint32_t guest_mxcsr_mask;
static uint64_t vcpu_serials;
static struct vfs_node *vm_node;
static struct vfs_node *vcpu_node;
static unsigned unknown_msr_reports;

// ---- small helpers ----

static inline uint64_t *vmcb64(struct kvm_vcpu *vcpu, unsigned offset) {
    return (uint64_t *)(vcpu->vmcb + offset);
}

static inline uint32_t *vmcb32(struct kvm_vcpu *vcpu, unsigned offset) {
    return (uint32_t *)(vcpu->vmcb + offset);
}

static inline void xsetbv0(uint64_t value) {
    __asm__ volatile("xsetbv" : : "a"((uint32_t)value), "d"((uint32_t)(value >> 32)), "c"(0U));
}

static inline uint64_t xgetbv0(void) {
    uint32_t low, high;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0U));
    return ((uint64_t)high << 32) | low;
}

static uint64_t alloc_zeroed_page(void) {
    void *page = pmm_alloc_page();
    if (!page) return 0;
    memset(vmm_phys_to_virt((uint64_t)(uintptr_t)page), 0, 4096);
    return (uint64_t)(uintptr_t)page;
}

static uint64_t get_reg(struct kvm_vcpu *vcpu, int reg) {
    if (reg == R_AX) return *vmcb64(vcpu, VMCB_RAX);
    if (reg == R_SP) return *vmcb64(vcpu, VMCB_RSP);
    return vcpu->regs[reg];
}

static void put_reg(struct kvm_vcpu *vcpu, int reg, uint64_t value) {
    if (reg == R_AX) *vmcb64(vcpu, VMCB_RAX) = value;
    else if (reg == R_SP) *vmcb64(vcpu, VMCB_RSP) = value;
    else vcpu->regs[reg] = value;
}

// A register write of an operand size: a 32-bit write clears the upper half, 8 and 16
// bits merge, and an 8-bit register 4-7 without REX is AH, CH, DH or BH.
static void write_reg_sized(struct kvm_vcpu *vcpu, int reg, int high_byte, unsigned size,
                            uint64_t value) {
    if (high_byte) {
        uint64_t old = get_reg(vcpu, reg);
        put_reg(vcpu, reg, (old & ~0xFF00ULL) | ((value & 0xFFULL) << 8));
        return;
    }
    uint64_t old = get_reg(vcpu, reg);
    switch (size) {
    case 1:  put_reg(vcpu, reg, (old & ~0xFFULL) | (value & 0xFFULL)); break;
    case 2:  put_reg(vcpu, reg, (old & ~0xFFFFULL) | (value & 0xFFFFULL)); break;
    case 4:  put_reg(vcpu, reg, value & 0xFFFFFFFFULL); break;
    default: put_reg(vcpu, reg, value); break;
    }
}

static uint64_t read_reg_sized(struct kvm_vcpu *vcpu, int reg, int high_byte, unsigned size) {
    uint64_t value = get_reg(vcpu, reg);
    if (high_byte) return (value >> 8) & 0xFFULL;
    if (size >= 8) return value;
    return value & ((1ULL << (size * 8U)) - 1ULL);
}

static uint64_t address_mask(unsigned address_size) {
    return address_size >= 8 ? ~0ULL : (1ULL << (address_size * 8U)) - 1ULL;
}

// A register used as an address or a count of the given address size, moved by delta.
static void add_reg_masked(struct kvm_vcpu *vcpu, int reg, unsigned address_size, int64_t delta) {
    uint64_t value = get_reg(vcpu, reg) + (uint64_t)delta;
    write_reg_sized(vcpu, reg, 0, address_size, value);
}

static void queue_exception(struct kvm_vcpu *vcpu, uint8_t vector, int has_error,
                            uint32_t error) {
    vcpu->exception_pending = 1;
    vcpu->exception_vector = vector;
    vcpu->exception_has_error = has_error;
    vcpu->exception_error = error;
}

// An instruction emulated here has completed, which also ends the interrupt shadow of an
// STI or MOV SS before it: after STI; HLT the interrupt must come right after the HLT.
static void advance_rip(struct kvm_vcpu *vcpu, uint64_t rip) {
    *vmcb64(vcpu, VMCB_RIP) = rip;
    *vmcb64(vcpu, VMCB_INT_STATE) &= ~1ULL;
}

static void skip_instruction(struct kvm_vcpu *vcpu) {
    advance_rip(vcpu, *vmcb64(vcpu, VMCB_NEXT_RIP));
}

// ---- memory slots and the nested page tables ----

static struct memory_slot *slot_for(struct kvm_vm *vm, uint64_t gpa) {
    for (uint32_t index = 0; index < vm->slot_count; index++) {
        struct memory_slot *slot = &vm->slots[index];
        if (gpa >= slot->gpa && gpa - slot->gpa < slot->size) return slot;
    }
    return NULL;
}

// The nested page table entry for a guest physical page, with the tables above it made
// when create is set.
static uint64_t *npt_entry(struct kvm_vm *vm, uint64_t gpa, int create) {
    uint64_t table = vm->npt_root;
    for (int level = 3; level > 0; level--) {
        uint64_t *entries = vmm_phys_to_virt(table);
        uint64_t *entry = &entries[(gpa >> (12 + 9 * level)) & 0x1FFULL];
        if (!(*entry & NPT_PRESENT)) {
            if (!create) return NULL;
            uint64_t next = alloc_zeroed_page();
            if (!next) return NULL;
            *entry = next | NPT_LEAF;
        }
        table = *entry & NPT_ADDRESS;
    }
    uint64_t *entries = vmm_phys_to_virt(table);
    return &entries[(gpa >> 12) & 0x1FFULL];
}

// Maps the page a nested page fault hit, and the pages around it in the same slot so a
// guest touching its memory in order does not exit on every page. Returns 1 when the
// address is in no slot: memory-mapped I/O.
static int npt_fault(struct kvm_vm *vm, uint64_t gpa) {
    struct memory_slot *slot = slot_for(vm, gpa);
    if (!slot) return 1;
    uint64_t page = gpa & ~0xFFFULL;
    uint64_t start = page & ~(FAULT_AROUND * 4096ULL - 1ULL);
    if (start < slot->gpa) start = slot->gpa;
    uint64_t end = start + FAULT_AROUND * 4096ULL;
    if (end > slot->gpa + slot->size) end = slot->gpa + slot->size;
    for (uint64_t at = start; at < end; at += 4096ULL) {
        uint64_t *entry = npt_entry(vm, at, 1);
        if (!entry) return -ENOMEM;
        if (*entry & NPT_PRESENT) continue;
        uint64_t physical;
        if (process_pin_user_page(slot->hva + (at - slot->gpa), &physical) != 0) {
            if (at == page) return -EFAULT;
            continue;
        }
        *entry = physical | NPT_LEAF;
    }
    return 0;
}

// Waits until no vCPU can still be running on translations from before the nested page
// tables changed: every vCPU is out of the guest or entered after the change.
static void vm_sync_vcpus(struct kvm_vm *vm) {
    uint64_t generation = __atomic_add_fetch(&vm->generation, 1, __ATOMIC_SEQ_CST);
    for (unsigned index = 0; index < KVM_MAX_VCPUS; index++) {
        struct kvm_vcpu *vcpu = vm->vcpus[index];
        if (!vcpu) continue;
        while (__atomic_load_n(&vcpu->in_guest, __ATOMIC_SEQ_CST) &&
               __atomic_load_n(&vcpu->entered_generation, __ATOMIC_SEQ_CST) != generation) {
            process_kernel_yield();
            cpu_relax();
        }
    }
}

// Takes a slot's pages out of the nested page tables. The entries keep their addresses
// until every vCPU has left the old translations; only then are the pages let go.
static void npt_unmap(struct kvm_vm *vm, uint64_t gpa, uint64_t size) {
    for (uint64_t at = gpa; at < gpa + size; at += 4096ULL) {
        uint64_t *entry = npt_entry(vm, at, 0);
        if (entry) *entry &= ~NPT_PRESENT;
    }
    vm_sync_vcpus(vm);
    for (uint64_t at = gpa; at < gpa + size; at += 4096ULL) {
        uint64_t *entry = npt_entry(vm, at, 0);
        if (!entry || !(*entry & NPT_ADDRESS)) continue;
        pmm_free_page((void *)(uintptr_t)(*entry & NPT_ADDRESS));
        *entry = 0;
    }
}

static void npt_free_table(uint64_t table, int level) {
    uint64_t *entries = vmm_phys_to_virt(table);
    for (unsigned index = 0; index < 512U; index++) {
        uint64_t address = entries[index] & NPT_ADDRESS;
        if (!address) continue;
        if (level > 0) npt_free_table(address, level - 1);
        else pmm_free_page((void *)(uintptr_t)address);
    }
    pmm_free_page((void *)(uintptr_t)table);
}

// ---- guest memory as the guest addresses it ----

static int guest_physical_read(struct kvm_vm *vm, uint64_t gpa, void *buffer, size_t size) {
    struct memory_slot *slot = slot_for(vm, gpa);
    if (!slot || size > slot->size - (gpa - slot->gpa)) return -EFAULT;
    return copy_from_user(buffer, slot->hva + (gpa - slot->gpa), size) == 0 ? 0 : -EFAULT;
}

static int guest_physical_write(struct kvm_vm *vm, uint64_t gpa, const void *buffer,
                                size_t size) {
    struct memory_slot *slot = slot_for(vm, gpa);
    if (!slot || size > slot->size - (gpa - slot->gpa)) return -EFAULT;
    return copy_to_user(slot->hva + (gpa - slot->gpa), buffer, size) == 0 ? 0 : -EFAULT;
}

// Walks the guest's own page tables: none, 32-bit, PAE or four-level.
static int guest_translate(struct kvm_vcpu *vcpu, uint64_t linear, uint64_t *gpa) {
    struct kvm_vm *vm = vcpu->vm;
    uint64_t cr0 = *vmcb64(vcpu, VMCB_CR0), cr3 = *vmcb64(vcpu, VMCB_CR3);
    uint64_t cr4 = *vmcb64(vcpu, VMCB_CR4), efer = *vmcb64(vcpu, VMCB_EFER);
    if (!(cr0 & CR0_PG)) {
        *gpa = (efer & EFER_LMA) ? linear : linear & 0xFFFFFFFFULL;
        return 0;
    }
    if (efer & EFER_LMA) {
        uint64_t table = cr3 & NPT_ADDRESS;
        for (int level = 3; level >= 0; level--) {
            uint64_t entry;
            if (guest_physical_read(vm, table + ((linear >> (12 + 9 * level)) & 0x1FFULL) * 8,
                                    &entry, 8) != 0 ||
                !(entry & 1))
                return -EFAULT;
            if (level > 0 && level < 3 && (entry & 0x80)) {
                uint64_t span = 1ULL << (12 + 9 * level);
                *gpa = (entry & NPT_ADDRESS & ~(span - 1ULL)) | (linear & (span - 1ULL));
                return 0;
            }
            table = entry & NPT_ADDRESS;
        }
        *gpa = table | (linear & 0xFFFULL);
        return 0;
    }
    linear &= 0xFFFFFFFFULL;
    if (cr4 & CR4_PAE) {
        uint64_t pdpte;
        if (guest_physical_read(vm, (cr3 & 0xFFFFFFE0ULL) + ((linear >> 30) & 3ULL) * 8, &pdpte,
                                8) != 0 ||
            !(pdpte & 1))
            return -EFAULT;
        uint64_t pde;
        if (guest_physical_read(vm, (pdpte & NPT_ADDRESS) + ((linear >> 21) & 0x1FFULL) * 8, &pde,
                                8) != 0 ||
            !(pde & 1))
            return -EFAULT;
        if (pde & 0x80) {
            *gpa = (pde & NPT_ADDRESS & ~0x1FFFFFULL) | (linear & 0x1FFFFFULL);
            return 0;
        }
        uint64_t pte;
        if (guest_physical_read(vm, (pde & NPT_ADDRESS) + ((linear >> 12) & 0x1FFULL) * 8, &pte,
                                8) != 0 ||
            !(pte & 1))
            return -EFAULT;
        *gpa = (pte & NPT_ADDRESS) | (linear & 0xFFFULL);
        return 0;
    }
    uint32_t pde;
    if (guest_physical_read(vm, (cr3 & 0xFFFFF000ULL) + ((linear >> 22) & 0x3FFULL) * 4, &pde, 4) !=
            0 ||
        !(pde & 1))
        return -EFAULT;
    if ((pde & 0x80) && (cr4 & CR4_PSE)) {
        *gpa = ((uint64_t)(pde & 0xFFC00000U) | ((uint64_t)(pde & 0x1FE000U) << 19)) |
            (linear & 0x3FFFFFULL);
        return 0;
    }
    uint32_t pte;
    if (guest_physical_read(vm, (pde & 0xFFFFF000U) + ((linear >> 12) & 0x3FFULL) * 4, &pte, 4) !=
            0 ||
        !(pte & 1))
        return -EFAULT;
    *gpa = (pte & 0xFFFFF000U) | (linear & 0xFFFULL);
    return 0;
}

static int guest_linear_access(struct kvm_vcpu *vcpu, uint64_t linear, void *buffer, size_t size,
                               int write) {
    uint8_t *bytes = buffer;
    while (size) {
        size_t chunk = 4096U - (size_t)(linear & 0xFFFULL);
        if (chunk > size) chunk = size;
        uint64_t gpa;
        if (guest_translate(vcpu, linear, &gpa) != 0) return -EFAULT;
        int status = write ? guest_physical_write(vcpu->vm, gpa, bytes, chunk)
                           : guest_physical_read(vcpu->vm, gpa, bytes, chunk);
        if (status != 0) return status;
        linear += chunk;
        bytes += chunk;
        size -= chunk;
    }
    return 0;
}

// ---- guest modes and segments ----

static int long_mode_code(struct kvm_vcpu *vcpu) {
    uint16_t attributes = *(uint16_t *)(vcpu->vmcb + VMCB_CS + 2);
    return (*vmcb64(vcpu, VMCB_EFER) & EFER_LMA) && (attributes & SEG_L);
}

static unsigned default_size(struct kvm_vcpu *vcpu) {
    if (long_mode_code(vcpu)) return 4;
    if (!(*vmcb64(vcpu, VMCB_CR0) & CR0_PE) || (*vmcb64(vcpu, VMCB_RFLAGS) & RFLAGS_VM)) return 2;
    uint16_t attributes = *(uint16_t *)(vcpu->vmcb + VMCB_CS + 2);
    return (attributes & SEG_DB) ? 4 : 2;
}

// Segment numbers as instructions encode them: ES, CS, SS, DS, FS, GS.
static const unsigned segment_offsets[6] = {VMCB_ES, VMCB_CS, VMCB_SS, VMCB_DS, VMCB_FS, VMCB_GS};

static uint64_t segment_base(struct kvm_vcpu *vcpu, unsigned segment) {
    if (long_mode_code(vcpu) && segment < 4) return 0;
    return *vmcb64(vcpu, segment_offsets[segment] + 8);
}

// ---- the instruction decoder for memory-mapped I/O ----
//
// The guest touched an address no slot backs. Its instruction is fetched through its own
// page tables and decoded as far as an MMIO access needs: which register or immediate,
// how wide, and how long the instruction is. These are the forms drivers and firmware
// use for device registers: MOV in its encodings, MOVZX/MOVSX, and the string MOVS,
// STOS and LODS.

enum mmio_op { OP_STORE_REG, OP_STORE_IMM, OP_LOAD, OP_MOVS, OP_STOS, OP_LODS };

struct instruction {
    unsigned length;
    enum mmio_op op;
    unsigned size;
    unsigned dest_size;
    unsigned address_size;
    int reg;
    int high_byte;
    int sign_extend;
    int segment;
    int rep;
    uint64_t immediate;
};

static int fetch_instruction(struct kvm_vcpu *vcpu, uint8_t *bytes, unsigned *count) {
    uint64_t linear = segment_base(vcpu, 1) + *vmcb64(vcpu, VMCB_RIP);
    if (!long_mode_code(vcpu)) linear &= 0xFFFFFFFFULL;
    unsigned first = 4096U - (unsigned)(linear & 0xFFFULL);
    if (first > 15) first = 15;
    if (guest_linear_access(vcpu, linear, bytes, first, 0) != 0) return -EFAULT;
    *count = first;
    if (first < 15 && guest_linear_access(vcpu, linear + first, bytes + first, 15 - first, 0) == 0)
        *count = 15;
    return 0;
}

// The length of a ModRM operand, from the byte after the opcode; the register field and
// whether the operand is memory come back too.
static int modrm_length(const uint8_t *bytes, unsigned available, unsigned address_size,
                        unsigned *length, int *reg_field) {
    if (available < 1) return -1;
    uint8_t modrm = bytes[0];
    unsigned mod = modrm >> 6, rm = modrm & 7U;
    *reg_field = (modrm >> 3) & 7;
    if (mod == 3) return -1;
    unsigned used = 1;
    if (address_size == 2) {
        if (mod == 0 && rm == 6) used += 2;
        else if (mod == 1) used += 1;
        else if (mod == 2) used += 2;
    } else {
        if (rm == 4) {
            if (available < 2) return -1;
            uint8_t sib = bytes[1];
            used++;
            if (mod == 0 && (sib & 7U) == 5) used += 4;
        }
        if (mod == 0 && rm == 5) used += 4;
        else if (mod == 1) used += 1;
        else if (mod == 2) used += 4;
    }
    if (used > available) return -1;
    *length = used;
    return 0;
}

static int decode(struct kvm_vcpu *vcpu, const uint8_t *bytes, unsigned available,
                  struct instruction *out) {
    memset(out, 0, sizeof(*out));
    int long64 = long_mode_code(vcpu);
    unsigned size_default = default_size(vcpu);
    int operand_prefix = 0, address_prefix = 0, rex = 0;
    out->segment = -1;
    unsigned at = 0;
    for (; at < available; at++) {
        uint8_t byte = bytes[at];
        if (byte == 0x66) operand_prefix = 1;
        else if (byte == 0x67) address_prefix = 1;
        else if (byte == 0xF2 || byte == 0xF3) out->rep = 1;
        else if (byte == 0xF0) continue;
        else if (byte == 0x26) out->segment = 0;
        else if (byte == 0x2E) out->segment = 1;
        else if (byte == 0x36) out->segment = 2;
        else if (byte == 0x3E) out->segment = 3;
        else if (byte == 0x64) out->segment = 4;
        else if (byte == 0x65) out->segment = 5;
        else break;
        rex = 0;
    }
    if (long64 && at < available && (bytes[at] & 0xF0U) == 0x40) rex = bytes[at++];
    if (at >= available) return -1;

    unsigned operand_size;
    if (long64) operand_size = (rex & 8) ? 8 : operand_prefix ? 2 : 4;
    else operand_size = operand_prefix ? (size_default == 4 ? 2 : 4) : size_default;
    unsigned address_size;
    if (long64) address_size = address_prefix ? 4 : 8;
    else address_size = address_prefix ? (size_default == 4 ? 2 : 4) : size_default;
    out->address_size = address_size;

    uint8_t opcode = bytes[at++];
    int two_byte = 0;
    if (opcode == 0x0F) {
        if (at >= available) return -1;
        two_byte = 1;
        opcode = bytes[at++];
    }

    unsigned modrm_bytes = 0;
    int reg_field = 0;
    int byte_operand = 0;
    if (!two_byte) {
        switch (opcode) {
        case 0x88: case 0x89: case 0x8A: case 0x8B: case 0xC6: case 0xC7:
            if (modrm_length(bytes + at, available - at, address_size, &modrm_bytes,
                             &reg_field) != 0)
                return -1;
            byte_operand = !(opcode & 1);
            out->size = byte_operand ? 1 : operand_size;
            out->dest_size = out->size;
            out->reg = reg_field | ((rex & 4) ? 8 : 0);
            if (opcode == 0x88 || opcode == 0x89) out->op = OP_STORE_REG;
            else if (opcode == 0x8A || opcode == 0x8B) out->op = OP_LOAD;
            else {
                if (reg_field != 0) return -1;
                out->op = OP_STORE_IMM;
            }
            at += modrm_bytes;
            if (out->op == OP_STORE_IMM) {
                unsigned immediate_bytes = byte_operand ? 1 : operand_size == 2 ? 2 : 4;
                if (at + immediate_bytes > available) return -1;
                uint64_t value = 0;
                memcpy(&value, bytes + at, immediate_bytes);
                if (immediate_bytes == 4 && operand_size == 8)
                    value = (uint64_t)(int64_t)(int32_t)(uint32_t)value;
                out->immediate = value;
                at += immediate_bytes;
            }
            break;
        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
            byte_operand = !(opcode & 1);
            out->size = byte_operand ? 1 : operand_size;
            out->dest_size = out->size;
            out->reg = R_AX;
            out->op = opcode < 0xA2 ? OP_LOAD : OP_STORE_REG;
            if (at + address_size > available) return -1;
            at += address_size;
            break;
        case 0xA4: case 0xA5:
            out->op = OP_MOVS;
            out->size = (opcode & 1) ? operand_size : 1;
            break;
        case 0xAA: case 0xAB:
            out->op = OP_STOS;
            out->size = (opcode & 1) ? operand_size : 1;
            out->reg = R_AX;
            break;
        case 0xAC: case 0xAD:
            out->op = OP_LODS;
            out->size = (opcode & 1) ? operand_size : 1;
            out->dest_size = out->size;
            out->reg = R_AX;
            break;
        default:
            return -1;
        }
    } else {
        if (opcode != 0xB6 && opcode != 0xB7 && opcode != 0xBE && opcode != 0xBF) return -1;
        if (modrm_length(bytes + at, available - at, address_size, &modrm_bytes, &reg_field) != 0)
            return -1;
        at += modrm_bytes;
        out->op = OP_LOAD;
        out->size = (opcode & 1) ? 2 : 1;
        out->dest_size = operand_size;
        out->sign_extend = opcode >= 0xBE;
        out->reg = reg_field | ((rex & 4) ? 8 : 0);
    }
    if (byte_operand && !rex && out->reg >= 4 && out->reg < 8) {
        out->high_byte = 1;
        out->reg -= 4;
    }
    out->length = at;
    return 0;
}

static void vmcb_init_state(struct kvm_vcpu *vcpu);
static void vmcb_segment(struct kvm_vcpu *vcpu, unsigned offset, uint16_t selector,
                         uint16_t attributes, uint32_t limit, uint64_t base);
static int complete_userspace(struct kvm_vcpu *vcpu);
static void pit_check(struct kvm_vm *vm);
static uint64_t next_timer(const struct kvm_vcpu *vcpu);
static int pic_pending(struct kvm_vm *vm);

// ---- the local APIC, in the kernel ----
//
// With KVM_CAP_SPLIT_IRQCHIP each vCPU's local APIC is emulated here, as the guest sees
// it at its APIC base: the registers, the timer, IPIs between vCPUs including INIT and
// SIPI, and interrupt delivery from MSIs and irqfds. The PIC and the I/O APIC stay in
// userspace; the PIC's interrupt comes in with KVM_INTERRUPT, the I/O APIC's as MSIs, and
// the EOI of a level-triggered interrupt goes back out as KVM_EXIT_IOAPIC_EOI.

#define APIC_ID      0x020
#define APIC_LVR     0x030
#define APIC_TASKPRI 0x080
#define APIC_ARBPRI  0x090
#define APIC_PROCPRI 0x0A0
#define APIC_EOI     0x0B0
#define APIC_LDR     0x0D0
#define APIC_DFR     0x0E0
#define APIC_SPIV    0x0F0
#define APIC_ISR     0x100
#define APIC_TMR     0x180
#define APIC_IRR     0x200
#define APIC_ESR     0x280
#define APIC_LVTCMCI 0x2F0
#define APIC_ICR     0x300
#define APIC_ICR2    0x310
#define APIC_LVTT    0x320
#define APIC_LVTTHMR 0x330
#define APIC_LVTPC   0x340
#define APIC_LVT0    0x350
#define APIC_LVT1    0x360
#define APIC_LVTERR  0x370
#define APIC_TMICT   0x380
#define APIC_TMCCT   0x390
#define APIC_TDCR    0x3E0

#define APIC_VERSION      0x00050014U  /* an integrated APIC with six LVT entries */
#define APIC_LVT_MASKED   (1U << 16)
#define APIC_SPIV_ENABLED (1U << 8)
#define APIC_TIMER_PERIODIC (1U << 17)
#define APIC_BASE_BSP     (1ULL << 8)
#define APIC_BASE_EXTD    (1ULL << 10)
#define APIC_BASE_ENABLE  (1ULL << 11)

#define DELIVERY_FIXED   0U
#define DELIVERY_LOWEST  1U
#define DELIVERY_SMI     2U
#define DELIVERY_NMI     4U
#define DELIVERY_INIT    5U
#define DELIVERY_STARTUP 6U
#define DELIVERY_EXTINT  7U

#define PENDING_INIT 1U
#define PENDING_SIPI 2U

static inline uint32_t apic_get(const struct kvm_vcpu *vcpu, unsigned offset) {
    return __atomic_load_n((const uint32_t *)(vcpu->apic + offset), __ATOMIC_ACQUIRE);
}

static inline void apic_set(struct kvm_vcpu *vcpu, unsigned offset, uint32_t value) {
    __atomic_store_n((uint32_t *)(vcpu->apic + offset), value, __ATOMIC_RELEASE);
}

static inline void apic_set_vector(struct kvm_vcpu *vcpu, unsigned base, unsigned vector) {
    __atomic_fetch_or((uint32_t *)(vcpu->apic + base + (vector / 32U) * 16U), 1U << (vector % 32U),
                      __ATOMIC_SEQ_CST);
}

static inline void apic_clear_vector(struct kvm_vcpu *vcpu, unsigned base, unsigned vector) {
    __atomic_fetch_and((uint32_t *)(vcpu->apic + base + (vector / 32U) * 16U),
                       ~(1U << (vector % 32U)), __ATOMIC_SEQ_CST);
}

static inline int apic_test_vector(const struct kvm_vcpu *vcpu, unsigned base, unsigned vector) {
    return (apic_get(vcpu, base + (vector / 32U) * 16U) >> (vector % 32U)) & 1U;
}

// The highest vector set in the ISR, TMR or IRR, or -1.
static int apic_highest(const struct kvm_vcpu *vcpu, unsigned base) {
    for (int word = 7; word >= 0; word--) {
        uint32_t bits = apic_get(vcpu, base + (unsigned)word * 16U);
        if (bits) return word * 32 + 31 - __builtin_clz(bits);
    }
    return -1;
}

static int apic_hw_enabled(const struct kvm_vcpu *vcpu) {
    return (vcpu->apic_base & APIC_BASE_ENABLE) != 0;
}

static int apic_enabled(const struct kvm_vcpu *vcpu) {
    return apic_hw_enabled(vcpu) && (apic_get(vcpu, APIC_SPIV) & APIC_SPIV_ENABLED);
}

static uint32_t apic_ppr(const struct kvm_vcpu *vcpu) {
    uint32_t tpr = apic_get(vcpu, APIC_TASKPRI) & 0xFFU;
    int isr = apic_highest(vcpu, APIC_ISR);
    uint32_t isrv = isr < 0 ? 0 : (uint32_t)isr & 0xF0U;
    return (tpr & 0xF0U) >= isrv ? tpr : isrv;
}

// The interrupt the APIC would give the processor now, or -1.
static int apic_deliverable(const struct kvm_vcpu *vcpu) {
    if (!apic_enabled(vcpu)) return -1;
    int irr = apic_highest(vcpu, APIC_IRR);
    if (irr < 0 || ((uint32_t)irr & 0xF0U) <= (apic_ppr(vcpu) & 0xF0U)) return -1;
    return irr;
}

// Whether the PIC's interrupt can reach the processor: LINT0 unmasked as ExtINT, or the
// APIC switched off, which leaves the PIC wired straight in.
static int apic_accepts_pic(const struct kvm_vcpu *vcpu) {
    if (!vcpu->vm->irqchip || !apic_hw_enabled(vcpu)) return 1;
    uint32_t lvt0 = apic_get(vcpu, APIC_LVT0);
    return !(lvt0 & APIC_LVT_MASKED) && ((lvt0 >> 8) & 7U) == DELIVERY_EXTINT;
}

// Makes a vCPU notice an event: wakes it if halted, and interrupts the guest if it runs.
static void vcpu_kick(struct kvm_vcpu *vcpu) {
    __atomic_store_n(&vcpu->kick, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&vcpu->in_guest, __ATOMIC_SEQ_CST))
        smp_send_reschedule_to(__atomic_load_n(&vcpu->cpu, __ATOMIC_RELAXED));
    (void)process_wake_all(&vcpu->wake);
}

static void apic_accept(struct kvm_vcpu *vcpu, unsigned mode, unsigned vector, int level,
                        int trigger_level) {
    switch (mode) {
    case DELIVERY_FIXED:
    case DELIVERY_LOWEST:
        if (!apic_enabled(vcpu)) return;
        if (trigger_level) apic_set_vector(vcpu, APIC_TMR, vector);
        else apic_clear_vector(vcpu, APIC_TMR, vector);
        apic_set_vector(vcpu, APIC_IRR, vector);
        break;
    case DELIVERY_NMI:
        __atomic_store_n(&vcpu->nmi_pending, 1, __ATOMIC_SEQ_CST);
        break;
    case DELIVERY_INIT:
        // An INIT level de-assert does nothing on an integrated APIC.
        if (trigger_level && !level) return;
        __atomic_fetch_or(&vcpu->pending, PENDING_INIT, __ATOMIC_SEQ_CST);
        break;
    case DELIVERY_STARTUP:
        __atomic_store_n(&vcpu->sipi_vector, vector & 0xFFU, __ATOMIC_SEQ_CST);
        __atomic_fetch_or(&vcpu->pending, PENDING_SIPI, __ATOMIC_SEQ_CST);
        break;
    default:
        return;
    }
    vcpu_kick(vcpu);
}

static int apic_match(const struct kvm_vcpu *vcpu, uint32_t destination, int logical) {
    if (!apic_hw_enabled(vcpu)) return 0;
    if (!logical) return destination == 0xFFU || (apic_get(vcpu, APIC_ID) >> 24) == destination;
    if (destination == 0xFFU) return 1;
    uint32_t ldr = apic_get(vcpu, APIC_LDR) >> 24;
    uint32_t model = apic_get(vcpu, APIC_DFR) >> 28;
    if (model == 0xFU) return (ldr & destination) != 0;
    if (model == 0x0U) return (ldr >> 4) == (destination >> 4) && (ldr & destination & 0xFU);
    return 0;
}

// Delivers an interrupt to the vCPUs it names; the vm's irq_lock is held. Lowest priority
// goes to the first that matches. Returns how many took it.
static int deliver_locked(struct kvm_vm *vm, struct kvm_vcpu *source, unsigned shorthand,
                          uint32_t destination, int logical, unsigned mode, unsigned vector,
                          int level, int trigger_level) {
    int delivered = 0;
    for (unsigned index = 0; index < KVM_MAX_VCPUS; index++) {
        struct kvm_vcpu *target = vm->vcpus[index];
        if (!target) continue;
        int match;
        switch (shorthand) {
        case 1:  match = target == source; break;
        case 2:  match = 1; break;
        case 3:  match = target != source; break;
        default: match = apic_match(target, destination, logical); break;
        }
        if (!match) continue;
        apic_accept(target, mode, vector, level, trigger_level);
        delivered++;
        if (mode == DELIVERY_LOWEST) break;
    }
    return delivered;
}

static int deliver_msi(struct kvm_vm *vm, uint32_t address, uint32_t data) {
    if ((address & 0xFFF00000U) != 0xFEE00000U) return 0;
    return deliver_locked(vm, NULL, 0, (address >> 12) & 0xFFU, (address >> 2) & 1U,
                          (data >> 8) & 7U, data & 0xFFU, 1, (data >> 15) & 1U);
}

// The timer counts down at 1 GHz divided by the divide configuration, as KVM's does; the
// guest calibrates it against another clock.
static uint64_t apic_timer_divisor(const struct kvm_vcpu *vcpu) {
    uint32_t dcr = apic_get(vcpu, APIC_TDCR);
    uint32_t value = (dcr & 3U) | ((dcr & 8U) >> 1);
    return value == 7U ? 1ULL : 2ULL << value;
}

static void apic_timer_start(struct kvm_vcpu *vcpu) {
    uint32_t count = apic_get(vcpu, APIC_TMICT);
    if (!count) {
        vcpu->timer_deadline = 0;
        vcpu->timer_period = 0;
        return;
    }
    vcpu->timer_period = (uint64_t)count * apic_timer_divisor(vcpu);
    vcpu->timer_deadline = time_uptime_ns() + vcpu->timer_period;
}

static uint32_t apic_timer_current(const struct kvm_vcpu *vcpu) {
    if (!vcpu->timer_deadline) return 0;
    uint64_t now = time_uptime_ns();
    uint64_t remaining = vcpu->timer_deadline > now ? vcpu->timer_deadline - now : 0;
    return (uint32_t)(remaining / apic_timer_divisor(vcpu));
}

// Fires the timer if it has run out: its interrupt, unless masked, and the next period.
static void apic_timer_check(struct kvm_vcpu *vcpu) {
    if (!vcpu->timer_deadline) return;
    uint64_t now = time_uptime_ns();
    if (now < vcpu->timer_deadline) return;
    uint32_t lvt = apic_get(vcpu, APIC_LVTT);
    if (!(lvt & APIC_LVT_MASKED)) apic_accept(vcpu, DELIVERY_FIXED, lvt & 0xFFU, 1, 0);
    if ((lvt & APIC_TIMER_PERIODIC) && vcpu->timer_period) {
        vcpu->timer_deadline += vcpu->timer_period;
        if (vcpu->timer_deadline <= now) vcpu->timer_deadline = now + vcpu->timer_period;
    } else {
        vcpu->timer_deadline = 0;
    }
}

static void apic_reset(struct kvm_vcpu *vcpu) {
    uint32_t id = apic_get(vcpu, APIC_ID);
    memset(vcpu->apic, 0, 1024);
    apic_set(vcpu, APIC_ID, id);
    apic_set(vcpu, APIC_LVR, APIC_VERSION);
    apic_set(vcpu, APIC_DFR, 0xFFFFFFFFU);
    apic_set(vcpu, APIC_SPIV, 0xFFU);
    for (unsigned offset = APIC_LVTT; offset <= APIC_LVTERR; offset += 0x10U)
        apic_set(vcpu, offset, APIC_LVT_MASKED);
    apic_set(vcpu, APIC_LVTCMCI, APIC_LVT_MASKED);
    vcpu->timer_deadline = 0;
    vcpu->timer_period = 0;
}

static uint32_t apic_read(struct kvm_vcpu *vcpu, unsigned offset) {
    switch (offset) {
    case APIC_ARBPRI: return 0;
    case APIC_PROCPRI: return apic_ppr(vcpu);
    case APIC_EOI:     return 0;
    case APIC_TMCCT:   return apic_timer_current(vcpu);
    case APIC_ICR:     return apic_get(vcpu, APIC_ICR) & ~(1U << 12);
    default:           return offset < 0x400U ? apic_get(vcpu, offset) : 0;
    }
}

// The EOI of the interrupt in service. A level-triggered one is reported so the I/O APIC
// can see whether its line is still asserted: returns that vector, or -1.
static int apic_eoi(struct kvm_vcpu *vcpu) {
    int vector = apic_highest(vcpu, APIC_ISR);
    if (vector < 0) return -1;
    apic_clear_vector(vcpu, APIC_ISR, (unsigned)vector);
    if (!apic_test_vector(vcpu, APIC_TMR, (unsigned)vector)) return -1;
    return vector;
}

// A register write. Returns a level-triggered vector whose EOI userspace must see, or -1.
static int apic_write(struct kvm_vcpu *vcpu, unsigned offset, uint32_t value) {
    switch (offset) {
    case APIC_ID:      apic_set(vcpu, APIC_ID, value & 0xFF000000U); break;
    case APIC_TASKPRI: apic_set(vcpu, APIC_TASKPRI, value & 0xFFU); break;
    case APIC_EOI:     return apic_eoi(vcpu);
    case APIC_LDR:     apic_set(vcpu, APIC_LDR, value & 0xFF000000U); break;
    case APIC_DFR:     apic_set(vcpu, APIC_DFR, value | 0x0FFFFFFFU); break;
    case APIC_SPIV:
        apic_set(vcpu, APIC_SPIV, value & 0x3FFU);
        if (!(value & APIC_SPIV_ENABLED)) {
            for (unsigned at = APIC_LVTT; at <= APIC_LVTERR; at += 0x10U)
                apic_set(vcpu, at, apic_get(vcpu, at) | APIC_LVT_MASKED);
            apic_set(vcpu, APIC_LVTCMCI, apic_get(vcpu, APIC_LVTCMCI) | APIC_LVT_MASKED);
        }
        break;
    case APIC_ESR:     apic_set(vcpu, APIC_ESR, 0); break;
    case APIC_ICR2:    apic_set(vcpu, APIC_ICR2, value & 0xFF000000U); break;
    case APIC_ICR: {
        apic_set(vcpu, APIC_ICR, value & ~(1U << 12));
        unsigned mode = (value >> 8) & 7U;
        if (mode == DELIVERY_SMI || mode == DELIVERY_EXTINT) break;
        struct kvm_vm *vm = vcpu->vm;
        mutex_lock(&vm->irq_lock);
        (void)deliver_locked(vm, vcpu, (value >> 18) & 3U, apic_get(vcpu, APIC_ICR2) >> 24,
                             (value >> 11) & 1U, mode, value & 0xFFU, (value >> 14) & 1U,
                             (value >> 15) & 1U);
        mutex_unlock(&vm->irq_lock);
        break;
    }
    case APIC_LVTT: case APIC_LVTTHMR: case APIC_LVTPC: case APIC_LVT0: case APIC_LVT1:
    case APIC_LVTERR: case APIC_LVTCMCI: {
        uint32_t mask = offset == APIC_LVTT ? 0x700FFU : offset == APIC_LVTERR ? 0x100FFU
                                                                                 : 0x1A7FFU;
        if (offset == APIC_LVTT && ((value ^ apic_get(vcpu, APIC_LVTT)) & (3U << 17))) {
            vcpu->timer_deadline = 0;
            vcpu->timer_period = 0;
        }
        if (!(apic_get(vcpu, APIC_SPIV) & APIC_SPIV_ENABLED)) value |= APIC_LVT_MASKED;
        apic_set(vcpu, offset, value & (mask | APIC_LVT_MASKED));
        break;
    }
    case APIC_TMICT:
        apic_set(vcpu, APIC_TMICT, value);
        apic_timer_start(vcpu);
        break;
    case APIC_TDCR:    apic_set(vcpu, APIC_TDCR, value & 0xBU); break;
    default:           break;
    }
    return -1;
}

static int apic_page(const struct kvm_vcpu *vcpu, uint64_t gpa) {
    return vcpu->vm->irqchip && apic_hw_enabled(vcpu) &&
        (gpa & ~0xFFFULL) == (vcpu->apic_base & 0xFFFFFF000ULL);
}

// INIT puts a processor back to its state after RESET, except for its APIC ID and base,
// and leaves it waiting for a SIPI.
static void vcpu_init_signal(struct kvm_vcpu *vcpu) {
    uint64_t pat = *vmcb64(vcpu, VMCB_G_PAT);
    vmcb_init_state(vcpu);
    *vmcb64(vcpu, VMCB_G_PAT) = pat;
    memset(vcpu->regs, 0, sizeof(vcpu->regs));
    vcpu->reinject = 0;
    vcpu->exception_pending = 0;
    vcpu->irq_pending = 0;
    vcpu->nmi_masked = 0;
    vcpu->iret_rip = 0;
    vcpu->xcr0 = 1;
    arch_fpu_init(vcpu->guest_fpu);
    apic_reset(vcpu);
    // The bootstrap processor runs on from the reset vector; the others wait for a SIPI.
    vcpu->mp_state = (vcpu->apic_base & APIC_BASE_BSP) ? KVM_MP_STATE_RUNNABLE
                                                       : KVM_MP_STATE_INIT_RECEIVED;
    vcpu->flush = 1;
}

// What other vCPUs and the timer have done to this one, then whether it can run: a
// halted vCPU wakes for an interrupt it would take, an NMI, or INIT.
static int vcpu_runnable(struct kvm_vcpu *vcpu) {
    uint32_t pending = __atomic_exchange_n(&vcpu->pending, 0, __ATOMIC_SEQ_CST);
    if (pending & PENDING_INIT) vcpu_init_signal(vcpu);
    if ((pending & PENDING_SIPI) && vcpu->mp_state == KVM_MP_STATE_INIT_RECEIVED) {
        uint32_t vector = __atomic_load_n(&vcpu->sipi_vector, __ATOMIC_SEQ_CST);
        vmcb_segment(vcpu, VMCB_CS, (uint16_t)(vector << 8), 0x9B, 0xFFFF,
                     (uint64_t)vector << 12);
        *vmcb64(vcpu, VMCB_RIP) = 0;
        vcpu->mp_state = KVM_MP_STATE_RUNNABLE;
    }
    apic_timer_check(vcpu);
    if (vcpu->vm->pit_created) pit_check(vcpu->vm);
    if (vcpu->mp_state == KVM_MP_STATE_HALTED) {
        int interrupt = (*vmcb64(vcpu, VMCB_RFLAGS) & RFLAGS_IF) &&
            (vcpu->irq_pending || apic_deliverable(vcpu) >= 0 ||
             (apic_accepts_pic(vcpu) && pic_pending(vcpu->vm)));
        if (interrupt || __atomic_load_n(&vcpu->nmi_pending, __ATOMIC_SEQ_CST))
            vcpu->mp_state = KVM_MP_STATE_RUNNABLE;
    }
    return vcpu->mp_state == KVM_MP_STATE_RUNNABLE;
}

// Waits in the kernel while the vCPU is halted or waiting for a SIPI. Returns -EINTR for
// a signal, which takes KVM_RUN back to userspace.
static int vcpu_block(struct kvm_vcpu *vcpu) {
    while (!vcpu_runnable(vcpu)) {
        if (process_prepare_wait_interruptible(&vcpu->wake, next_timer(vcpu)) != 0)
            return -EINTR;
        if (vcpu_runnable(vcpu)) {
            process_finish_wait();
            break;
        }
        process_wait();
        process_finish_wait();
        if (process_signal_interrupts_wait()) return -EINTR;
    }
    return 0;
}

// ---- the PIC, I/O APIC and PIT, in the kernel ----
//
// With KVM_CREATE_IRQCHIP the whole interrupt controller is here: two 8259 PICs and an I/O
// APIC at 0xFEC00000, and with KVM_CREATE_PIT2 the 8254 timer, as on a PC. They behave as
// Linux's emulation of them does (arch/x86/kvm/i8259.c, ioapic.c, i8254.c), which follows
// the chips' datasheets. Their state is under the VM's irq_lock.

// ---- 8259 ----

static void pic_set_irq1(struct pic_chip *pic, unsigned irq, int level) {
    uint8_t mask = (uint8_t)(1U << irq);
    if (pic->elcr & mask) {
        if (level) {
            pic->irr |= mask;
            pic->last_irr |= mask;
        } else {
            pic->irr &= (uint8_t)~mask;
            pic->last_irr &= (uint8_t)~mask;
        }
    } else if (level) {
        if (!(pic->last_irr & mask)) pic->irr |= mask;
        pic->last_irr |= mask;
    } else {
        pic->last_irr &= (uint8_t)~mask;
    }
}

static int pic_priority(const struct pic_chip *pic, uint8_t mask) {
    if (!mask) return 8;
    int priority = 0;
    while (!(mask & (1U << ((priority + pic->priority_add) & 7)))) priority++;
    return priority;
}

static int pic_get_irq(const struct pic_chip *pic, int master) {
    int priority = pic_priority(pic, pic->irr & (uint8_t)~pic->imr);
    if (priority == 8) return -1;
    uint8_t mask = pic->isr;
    if (pic->special_mask) mask &= (uint8_t)~pic->imr;
    if (pic->special_fully_nested_mode && master) mask &= (uint8_t)~(1U << 2);
    int current = pic_priority(pic, mask);
    return priority < current ? (priority + pic->priority_add) & 7 : -1;
}

// Cascades the slave into the master's IRQ2. Returns whether the master's INTR is up.
static int pic_update(struct kvm_vm *vm) {
    if (pic_get_irq(&vm->pic[1], 0) >= 0) {
        pic_set_irq1(&vm->pic[0], 2, 1);
        pic_set_irq1(&vm->pic[0], 2, 0);
    }
    return pic_get_irq(&vm->pic[0], 1) >= 0;
}

// The PIC's INTR goes to whichever vCPU takes ExtINT on LINT0.
static void pic_notify(struct kvm_vm *vm) {
    if (!pic_update(vm)) return;
    for (unsigned index = 0; index < KVM_MAX_VCPUS; index++) {
        struct kvm_vcpu *vcpu = vm->vcpus[index];
        if (vcpu && apic_accepts_pic(vcpu)) vcpu_kick(vcpu);
    }
}

static void pic_intack(struct pic_chip *pic, unsigned irq) {
    if (pic->auto_eoi) {
        if (pic->rotate_on_auto_eoi) pic->priority_add = (uint8_t)((irq + 1) & 7);
    } else {
        pic->isr |= (uint8_t)(1U << irq);
    }
    if (!(pic->elcr & (1U << irq))) pic->irr &= (uint8_t)~(1U << irq);
}

// The interrupt acknowledge cycle: the vector of the highest request, which goes in
// service. A request gone by then reads as the spurious IRQ7.
static unsigned pic_read_irq(struct kvm_vm *vm) {
    struct pic_chip *master = &vm->pic[0], *slave = &vm->pic[1];
    unsigned vector;
    int irq = pic_get_irq(master, 1);
    if (irq >= 0) {
        pic_intack(master, (unsigned)irq);
        if (irq == 2) {
            int irq2 = pic_get_irq(slave, 0);
            if (irq2 >= 0) pic_intack(slave, (unsigned)irq2);
            else irq2 = 7;
            vector = slave->irq_base + (unsigned)irq2;
        } else {
            vector = master->irq_base + (unsigned)irq;
        }
    } else {
        vector = master->irq_base + 7U;
    }
    (void)pic_update(vm);
    return vector & 0xFFU;
}

static void pic_write(struct kvm_vm *vm, unsigned chip, unsigned port, uint8_t value) {
    struct pic_chip *pic = &vm->pic[chip];
    if (!(port & 1)) {
        if (value & 0x10) {
            // ICW1
            pic->init4 = value & 1;
            pic->last_irr = 0;
            pic->irr &= pic->elcr;
            pic->imr = 0;
            pic->priority_add = 0;
            pic->special_mask = 0;
            pic->read_reg_select = 0;
            if (!pic->init4) {
                pic->special_fully_nested_mode = 0;
                pic->auto_eoi = 0;
            }
            pic->init_state = 1;
        } else if (value & 0x08) {
            // OCW3
            if (value & 0x04) pic->poll = 1;
            if (value & 0x02) pic->read_reg_select = value & 1;
            if (value & 0x40) pic->special_mask = (value >> 5) & 1;
        } else {
            // OCW2
            unsigned command = value >> 5;
            if (command == 0 || command == 4) {
                pic->rotate_on_auto_eoi = (uint8_t)(command >> 2);
            } else if (command == 1 || command == 5) {
                int priority = pic_priority(pic, pic->isr);
                if (priority != 8) {
                    unsigned irq = (unsigned)(priority + pic->priority_add) & 7U;
                    if (command == 5) pic->priority_add = (uint8_t)((irq + 1) & 7);
                    pic->isr &= (uint8_t)~(1U << irq);
                }
            } else if (command == 3) {
                pic->isr &= (uint8_t)~(1U << (value & 7));
            } else if (command == 6) {
                pic->priority_add = (uint8_t)((value + 1) & 7);
            } else if (command == 7) {
                unsigned irq = value & 7U;
                pic->priority_add = (uint8_t)((irq + 1) & 7);
                pic->isr &= (uint8_t)~(1U << irq);
            }
        }
    } else if (pic->init_state == 0) {
        pic->imr = value;
    } else if (pic->init_state == 1) {
        pic->irq_base = value & 0xF8U;
        pic->init_state = 2;
    } else if (pic->init_state == 2) {
        pic->init_state = pic->init4 ? 3 : 0;
    } else {
        pic->special_fully_nested_mode = (value >> 4) & 1;
        pic->auto_eoi = (value >> 1) & 1;
        pic->init_state = 0;
    }
    pic_notify(vm);
}

static uint8_t pic_read(struct kvm_vm *vm, unsigned chip, unsigned port) {
    struct pic_chip *pic = &vm->pic[chip];
    if (pic->poll) {
        pic->poll = 0;
        int irq = pic_get_irq(pic, chip == 0);
        if (irq < 0) {
            (void)pic_update(vm);
            return 0x07;
        }
        if (chip) {
            vm->pic[0].isr &= (uint8_t)~(1U << 2);
            vm->pic[0].irr &= (uint8_t)~(1U << 2);
        }
        pic->irr &= (uint8_t)~(1U << irq);
        pic->isr &= (uint8_t)~(1U << irq);
        (void)pic_update(vm);
        return (uint8_t)irq;
    }
    if (!(port & 1)) return pic->read_reg_select ? pic->isr : pic->irr;
    return pic->imr;
}

// ---- I/O APIC ----

static void ioapic_service(struct kvm_vm *vm, unsigned pin) {
    uint64_t entry = vm->ioapic.redirection[pin];
    if (entry & IOAPIC_MASKED) return;
    int level = (entry & IOAPIC_LEVEL) != 0;
    if (level && (entry & IOAPIC_REMOTE_IRR)) return;
    (void)deliver_locked(vm, NULL, 0, (uint32_t)(entry >> 56), (entry >> 11) & 1U,
                         (entry >> 8) & 7U, entry & 0xFFU, 1, level);
    if (level) vm->ioapic.redirection[pin] |= IOAPIC_REMOTE_IRR;
}

static void ioapic_set_irq(struct kvm_vm *vm, unsigned pin, int level) {
    if (pin >= KVM_IOAPIC_NUM_PINS) return;
    uint32_t mask = 1U << pin;
    if (vm->ioapic.redirection[pin] & IOAPIC_LEVEL) {
        if (level) {
            vm->ioapic.irr |= mask;
            ioapic_service(vm, pin);
        } else {
            vm->ioapic.irr &= ~mask;
        }
    } else if (level && !(vm->ioapic.irr & mask)) {
        vm->ioapic.irr |= mask;
        ioapic_service(vm, pin);
    } else if (!level) {
        vm->ioapic.irr &= ~mask;
    }
}

// A local APIC's EOI of a level-triggered vector: a pin still asserted raises it again.
static void ioapic_eoi(struct kvm_vm *vm, unsigned vector) {
    for (unsigned pin = 0; pin < KVM_IOAPIC_NUM_PINS; pin++) {
        uint64_t *entry = &vm->ioapic.redirection[pin];
        if ((*entry & 0xFFU) != vector || !(*entry & IOAPIC_LEVEL)) continue;
        *entry &= ~IOAPIC_REMOTE_IRR;
        if (vm->ioapic.irr & (1U << pin)) ioapic_service(vm, pin);
    }
}

static uint32_t ioapic_read(struct kvm_vm *vm) {
    uint32_t select = vm->ioapic.ioregsel;
    if (select == 0 || select == 2) return vm->ioapic.id << 24;
    if (select == 1) return ((KVM_IOAPIC_NUM_PINS - 1U) << 16) | IOAPIC_VERSION;
    if (select >= 0x10 && select < 0x10 + KVM_IOAPIC_NUM_PINS * 2) {
        uint64_t entry = vm->ioapic.redirection[(select - 0x10) / 2];
        return (select & 1) ? (uint32_t)(entry >> 32) : (uint32_t)entry;
    }
    return 0xFFFFFFFFU;
}

static void ioapic_write(struct kvm_vm *vm, uint32_t value) {
    uint32_t select = vm->ioapic.ioregsel;
    if (select == 0) {
        vm->ioapic.id = (value >> 24) & 0xFU;
        return;
    }
    if (select < 0x10 || select >= 0x10 + KVM_IOAPIC_NUM_PINS * 2) return;
    unsigned pin = (select - 0x10) / 2;
    uint64_t *entry = &vm->ioapic.redirection[pin];
    // Delivery status and remote IRR are the chip's to set.
    uint64_t kept = *entry & (IOAPIC_DELIVERING | IOAPIC_REMOTE_IRR);
    if (select & 1) *entry = (*entry & 0xFFFFFFFFULL) | ((uint64_t)value << 32);
    else *entry = (*entry & ~0xFFFFFFFFULL) | value;
    *entry = (*entry & ~(IOAPIC_DELIVERING | IOAPIC_REMOTE_IRR)) | kept;
    if (!(*entry & IOAPIC_LEVEL)) *entry &= ~IOAPIC_REMOTE_IRR;
    if ((*entry & IOAPIC_LEVEL) && (vm->ioapic.irr & (1U << pin))) ioapic_service(vm, pin);
}

// IOREGSEL at 0x00 and IOWIN at 0x10, as whole 32-bit accesses.
static int ioapic_mmio(struct kvm_vcpu *vcpu, uint64_t gpa, int write, uint64_t value) {
    struct kvm_vm *vm = vcpu->vm;
    unsigned offset = (unsigned)(gpa & 0xFFFULL);
    uint64_t data = 0;
    mutex_lock(&vm->irq_lock);
    if (offset == 0x00) {
        if (write) vm->ioapic.ioregsel = (uint32_t)value & 0xFFU;
        else data = vm->ioapic.ioregsel;
    } else if (offset == 0x10) {
        if (write) ioapic_write(vm, (uint32_t)value);
        else data = ioapic_read(vm);
    }
    mutex_unlock(&vm->irq_lock);
    if (write) return 0;
    memcpy(vcpu->run->mmio.data, &data, sizeof(data));
    return complete_userspace(vcpu) == 0 ? 0 : -EFAULT;
}

// ---- GSIs into the chips ----

// A GSI at a level, through its routes: PIC and I/O APIC pins, or an MSI when raised. The
// irq_lock is held. Returns how many routes took it.
static int set_gsi_locked(struct kvm_vm *vm, uint32_t gsi, int level) {
    int delivered = 0, pic_changed = 0;
    for (uint32_t index = 0; index < vm->route_count; index++) {
        const struct kvm_irq_routing_entry *route = &vm->routes[index];
        if (route->gsi != gsi) continue;
        if (route->type == KVM_IRQ_ROUTING_MSI) {
            if (level) delivered += deliver_msi(vm, route->u.msi.address_lo, route->u.msi.data);
        } else if (vm->irqchip == IRQCHIP_FULL) {
            uint32_t chip = route->u.irqchip.irqchip, pin = route->u.irqchip.pin;
            if (chip <= KVM_IRQCHIP_PIC_SLAVE && pin < 8) {
                pic_set_irq1(&vm->pic[chip], pin, level);
                pic_changed = 1;
            } else if (chip == KVM_IRQCHIP_IOAPIC) {
                ioapic_set_irq(vm, pin, level);
            }
            delivered++;
        }
    }
    if (pic_changed) pic_notify(vm);
    return delivered;
}

// The routes a VM starts with: GSIs 0-15 on the PICs, and all 24 on the I/O APIC.
static int default_routes(struct kvm_vm *vm) {
    uint32_t count = 16 + KVM_IOAPIC_NUM_PINS;
    struct kvm_irq_routing_entry *routes = kmalloc(count * sizeof(*routes));
    if (!routes) return -ENOMEM;
    memset(routes, 0, count * sizeof(*routes));
    for (uint32_t gsi = 0; gsi < 16; gsi++) {
        routes[gsi].gsi = gsi;
        routes[gsi].type = KVM_IRQ_ROUTING_IRQCHIP;
        routes[gsi].u.irqchip.irqchip = gsi < 8 ? KVM_IRQCHIP_PIC_MASTER : KVM_IRQCHIP_PIC_SLAVE;
        routes[gsi].u.irqchip.pin = gsi & 7U;
    }
    for (uint32_t pin = 0; pin < KVM_IOAPIC_NUM_PINS; pin++) {
        routes[16 + pin].gsi = pin;
        routes[16 + pin].type = KVM_IRQ_ROUTING_IRQCHIP;
        routes[16 + pin].u.irqchip.irqchip = KVM_IRQCHIP_IOAPIC;
        routes[16 + pin].u.irqchip.pin = pin;
    }
    kfree(vm->routes);
    vm->routes = routes;
    vm->route_count = count;
    return 0;
}

// ---- 8254 ----

#define RW_LSB   1U
#define RW_MSB   2U
#define RW_WORD0 3U
#define RW_WORD1 4U

static uint64_t pit_ticks(const struct pit_channel *channel) {
    uint64_t now = time_uptime_ns();
    uint64_t ns = now > channel->load_time ? now - channel->load_time : 0;
    return ns / 1000000000ULL * PIT_FREQUENCY + (ns % 1000000000ULL) * PIT_FREQUENCY / 1000000000ULL;
}

static uint64_t pit_ns(uint64_t ticks) {
    return ticks / PIT_FREQUENCY * 1000000000ULL + (ticks % PIT_FREQUENCY) * 1000000000ULL / PIT_FREQUENCY;
}

static uint32_t pit_count(const struct pit_channel *channel) {
    uint64_t elapsed = pit_ticks(channel);
    switch (channel->mode) {
    case 0: case 1: case 4: case 5: return (uint32_t)((channel->count - elapsed) & 0xFFFFU);
    case 3:  return (uint32_t)(channel->count - (2 * elapsed) % channel->count);
    default: return (uint32_t)(channel->count - elapsed % channel->count);
    }
}

static int pit_out(const struct pit_channel *channel) {
    uint64_t elapsed = pit_ticks(channel);
    switch (channel->mode) {
    case 0:  return elapsed >= channel->count;
    case 1:  return elapsed < channel->count;
    case 2:  return elapsed % channel->count == 0 && elapsed != 0;
    case 3:  return elapsed % channel->count < (channel->count + 1) / 2;
    default: return elapsed == channel->count;
    }
}

// Channel 0 raises IRQ0 every period in modes 2 and 3, and once at zero in the others.
static void pit_load(struct kvm_vm *vm, unsigned index, uint32_t value) {
    struct pit_channel *channel = &vm->pit.channels[index];
    channel->count = value ? value : 0x10000;
    channel->load_time = time_uptime_ns();
    if (index == 0)
        __atomic_store_n(&vm->pit.deadline, channel->load_time + pit_ns(channel->count),
                         __ATOMIC_RELEASE);
}

static void pit_latch_count(struct pit_channel *channel) {
    if (channel->count_latched) return;
    channel->latched_count = (uint16_t)pit_count(channel);
    channel->count_latched = channel->rw_mode;
}

static void pit_latch_status(struct pit_channel *channel) {
    if (channel->status_latched) return;
    channel->status = (uint8_t)((pit_out(channel) << 7) | (channel->rw_mode << 4) |
                                (channel->mode << 1) | channel->bcd);
    channel->status_latched = 1;
}

static void pit_write(struct kvm_vm *vm, unsigned port, uint8_t value) {
    unsigned address = port & 3U;
    if (address == 3) {
        unsigned index = value >> 6;
        if (index == 3) {
            // Read-back: latch the count and status of the channels it names.
            for (unsigned at = 0; at < 3; at++) {
                if (!(value & (2U << at))) continue;
                if (!(value & 0x20)) pit_latch_count(&vm->pit.channels[at]);
                if (!(value & 0x10)) pit_latch_status(&vm->pit.channels[at]);
            }
            return;
        }
        struct pit_channel *channel = &vm->pit.channels[index];
        unsigned access = (value >> 4) & 3U;
        if (!access) {
            pit_latch_count(channel);
            return;
        }
        channel->rw_mode = channel->read_state = channel->write_state = (uint8_t)access;
        channel->mode = (value >> 1) & 7U;
        if (channel->mode > 5) channel->mode -= 4;
        channel->bcd = value & 1;
        return;
    }
    struct pit_channel *channel = &vm->pit.channels[address];
    if (channel->write_state == RW_MSB) {
        pit_load(vm, address, (uint32_t)value << 8);
    } else if (channel->write_state == RW_WORD0) {
        channel->write_latch = value;
        channel->write_state = RW_WORD1;
    } else if (channel->write_state == RW_WORD1) {
        pit_load(vm, address, channel->write_latch | ((uint32_t)value << 8));
        channel->write_state = RW_WORD0;
    } else {
        pit_load(vm, address, value);
    }
}

static uint8_t pit_read(struct kvm_vm *vm, unsigned port) {
    struct pit_channel *channel = &vm->pit.channels[port & 3U];
    if (channel->status_latched) {
        channel->status_latched = 0;
        return channel->status;
    }
    if (channel->count_latched) {
        uint8_t value = (uint8_t)channel->latched_count;
        if (channel->count_latched == RW_MSB) {
            value = (uint8_t)(channel->latched_count >> 8);
            channel->count_latched = 0;
        } else if (channel->count_latched == RW_WORD0) {
            channel->count_latched = RW_MSB;
        } else {
            channel->count_latched = 0;
        }
        return value;
    }
    uint32_t count = pit_count(channel);
    if (channel->read_state == RW_MSB) return (uint8_t)(count >> 8);
    if (channel->read_state == RW_WORD0) {
        channel->read_state = RW_WORD1;
        return (uint8_t)count;
    }
    if (channel->read_state == RW_WORD1) {
        channel->read_state = RW_WORD0;
        return (uint8_t)(count >> 8);
    }
    return (uint8_t)count;
}

// Port 0x61: the speaker's data bit, channel 2's gate and output, and the refresh toggle.
static uint8_t speaker_read(struct kvm_vm *vm) {
    uint64_t refresh = (time_uptime_ns() >> 14) & 1ULL;
    return (uint8_t)((vm->pit.speaker_data << 1) | vm->pit.channels[2].gate |
                     (pit_out(&vm->pit.channels[2]) << 5) | (refresh << 4));
}

static void speaker_write(struct kvm_vm *vm, uint8_t value) {
    struct pit_channel *channel = &vm->pit.channels[2];
    uint8_t gate = value & 1;
    vm->pit.speaker_data = (value >> 1) & 1;
    // A rising gate restarts counting in the modes it triggers.
    if (channel->mode != 0 && channel->mode != 4 && gate > channel->gate)
        channel->load_time = time_uptime_ns();
    channel->gate = gate;
}

static void pit_reset(struct kvm_vm *vm) {
    memset(&vm->pit, 0, sizeof(vm->pit));
    for (unsigned index = 0; index < 3; index++) {
        vm->pit.channels[index].gate = index != 2;
        pit_load(vm, index, 0);
    }
    vm->pit.deadline = 0;
}

// Raises IRQ0 if channel 0 has run out and sets its next deadline. Every vCPU checks it
// when it runs or wakes.
static void pit_check(struct kvm_vm *vm) {
    uint64_t deadline = __atomic_load_n(&vm->pit.deadline, __ATOMIC_ACQUIRE);
    if (!deadline || time_uptime_ns() < deadline) return;
    mutex_lock(&vm->irq_lock);
    deadline = vm->pit.deadline;
    uint64_t now = time_uptime_ns();
    if (deadline && now >= deadline) {
        struct pit_channel *channel = &vm->pit.channels[0];
        (void)set_gsi_locked(vm, 0, 1);
        (void)set_gsi_locked(vm, 0, 0);
        uint64_t next = 0;
        if (channel->mode == 2 || channel->mode == 3) {
            uint64_t period = pit_ns(channel->count);
            next = deadline + period;
            if (next <= now) next = now + period;
        }
        __atomic_store_n(&vm->pit.deadline, next, __ATOMIC_RELEASE);
    }
    mutex_unlock(&vm->irq_lock);
}

// The earliest of this vCPU's APIC timer and the PIT, or 0.
static uint64_t next_timer(const struct kvm_vcpu *vcpu) {
    uint64_t apic = vcpu->timer_deadline;
    uint64_t pit = vcpu->vm->pit_created ? __atomic_load_n(&vcpu->vm->pit.deadline, __ATOMIC_ACQUIRE) : 0;
    if (!apic) return pit;
    if (!pit) return apic;
    return apic < pit ? apic : pit;
}

// Port I/O the chips answer. Returns 1 if it was theirs.
static int chips_io(struct kvm_vcpu *vcpu, uint16_t port, int in, unsigned size, uint8_t *data) {
    struct kvm_vm *vm = vcpu->vm;
    int pic = vm->irqchip == IRQCHIP_FULL &&
        (port == 0x20 || port == 0x21 || port == 0xA0 || port == 0xA1 || port == 0x4D0 ||
         port == 0x4D1);
    int pit = vm->pit_created && ((port >= 0x40 && port <= 0x43) || port == 0x61);
    if ((!pic && !pit) || size != 1) return 0;
    mutex_lock(&vm->irq_lock);
    if (port == 0x4D0 || port == 0x4D1) {
        struct pic_chip *chip = &vm->pic[port & 1];
        if (in) *data = chip->elcr;
        else chip->elcr = *data & chip->elcr_mask;
    } else if (pic) {
        unsigned chip = port >= 0xA0;
        if (in) *data = pic_read(vm, chip, port);
        else pic_write(vm, chip, port, *data);
    } else if (port == 0x61) {
        if (in) *data = speaker_read(vm);
        else speaker_write(vm, *data);
    } else {
        if (in) *data = port == 0x43 ? 0xFF : pit_read(vm, port);
        else pit_write(vm, port, *data);
    }
    mutex_unlock(&vm->irq_lock);
    return 1;
}

static int pic_pending(struct kvm_vm *vm) {
    if (vm->irqchip != IRQCHIP_FULL) return 0;
    mutex_lock(&vm->irq_lock);
    int pending = pic_get_irq(&vm->pic[0], 1) >= 0;
    mutex_unlock(&vm->irq_lock);
    return pending;
}

// The PIC's interrupt, acknowledged, or -1.
static int pic_take(struct kvm_vm *vm) {
    mutex_lock(&vm->irq_lock);
    int vector = pic_get_irq(&vm->pic[0], 1) >= 0 ? (int)pic_read_irq(vm) : -1;
    mutex_unlock(&vm->irq_lock);
    return vector;
}

// ---- interrupt routing and irqfds ----

struct irqfd {
    struct irqfd *next;
    struct kvm_vm *vm;
    uint32_t gsi;
    struct file *file;
};

// An irqfd's GSI: an edge, raised and lowered again, as Linux's irqfd gives it.
static int raise_gsi(struct kvm_vm *vm, uint32_t gsi) {
    mutex_lock(&vm->irq_lock);
    int delivered = set_gsi_locked(vm, gsi, 1);
    (void)set_gsi_locked(vm, gsi, 0);
    mutex_unlock(&vm->irq_lock);
    return delivered;
}

static void irqfd_fire(void *argument) {
    struct irqfd *irqfd = argument;
    (void)raise_gsi(irqfd->vm, irqfd->gsi);
}

static int64_t set_irqfd(struct kvm_vm *vm, uint64_t argument) {
    struct kvm_irqfd request;
    if (copy_from_user(&request, argument, sizeof(request)) != 0) return -EFAULT;
    if (!vm->irqchip) return -EINVAL;
    if (request.flags & ~(uint32_t)KVM_IRQFD_FLAG_DEASSIGN) return -EINVAL;
    struct file *file = process_file_get(process_current(), (int)request.fd);
    if (!file) return -EBADF;
    if (file->kind != FILE_KIND_EVENTFD || !file->eventfd) {
        file_unref(file);
        return -EINVAL;
    }
    int64_t status = 0;
    struct irqfd *found = NULL;
    mutex_lock(&vm->irq_lock);
    struct irqfd **link = &vm->irqfds;
    for (; *link; link = &(*link)->next)
        if ((*link)->file == file && (*link)->gsi == request.gsi) break;
    if (request.flags & KVM_IRQFD_FLAG_DEASSIGN) {
        if (*link) {
            found = *link;
            *link = found->next;
        } else {
            status = -ENOENT;
        }
    } else if (*link) {
        status = -EBUSY;
    } else {
        struct irqfd *irqfd = kmalloc(sizeof(*irqfd));
        if (!irqfd) {
            status = -ENOMEM;
        } else {
            *irqfd = (struct irqfd){.next = vm->irqfds, .vm = vm, .gsi = request.gsi, .file = file};
            vm->irqfds = irqfd;
            file_ref(file);
            eventfd_set_consumer(file->eventfd, irqfd_fire, irqfd);
        }
    }
    mutex_unlock(&vm->irq_lock);
    if (found) {
        eventfd_set_consumer(found->file->eventfd, NULL, NULL);
        file_unref(found->file);
        kfree(found);
    }
    file_unref(file);
    return status;
}

static int64_t set_gsi_routing(struct kvm_vm *vm, uint64_t argument) {
    struct kvm_irq_routing header;
    if (copy_from_user(&header, argument, sizeof(header)) != 0) return -EFAULT;
    if (header.flags || header.nr > KVM_MAX_ROUTES) return -EINVAL;
    size_t bytes = (size_t)header.nr * sizeof(struct kvm_irq_routing_entry);
    struct kvm_irq_routing_entry *routes = header.nr ? kmalloc(bytes) : NULL;
    if (header.nr && !routes) return -ENOMEM;
    if (bytes && copy_from_user(routes, argument + sizeof(header), bytes) != 0) {
        kfree(routes);
        return -EFAULT;
    }
    for (uint32_t index = 0; index < header.nr; index++) {
        const struct kvm_irq_routing_entry *route = &routes[index];
        int known = route->type == KVM_IRQ_ROUTING_MSI ||
            (route->type == KVM_IRQ_ROUTING_IRQCHIP && route->u.irqchip.irqchip <= 2);
        if (route->gsi >= KVM_MAX_ROUTES || !known || route->flags) {
            kfree(routes);
            return -EINVAL;
        }
    }
    mutex_lock(&vm->irq_lock);
    struct kvm_irq_routing_entry *old = vm->routes;
    vm->routes = routes;
    vm->route_count = header.nr;
    mutex_unlock(&vm->irq_lock);
    kfree(old);
    return 0;
}

static int64_t signal_msi(struct kvm_vm *vm, uint64_t argument) {
    struct kvm_msi msi;
    if (copy_from_user(&msi, argument, sizeof(msi)) != 0) return -EFAULT;
    if (!vm->irqchip) return -ENXIO;
    if (msi.flags) return -EINVAL;
    mutex_lock(&vm->irq_lock);
    int delivered = deliver_msi(vm, msi.address_lo, msi.data);
    mutex_unlock(&vm->irq_lock);
    return delivered;
}

static int64_t irq_line(struct kvm_vm *vm, uint64_t argument, int with_status) {
    struct kvm_irq_level line;
    if (copy_from_user(&line, argument, sizeof(line)) != 0) return -EFAULT;
    if (!vm->irqchip) return -ENXIO;
    mutex_lock(&vm->irq_lock);
    int delivered = set_gsi_locked(vm, line.irq, line.level);
    mutex_unlock(&vm->irq_lock);
    if (with_status) {
        line.irq = (uint32_t)delivered;
        if (copy_to_user(argument, &line, sizeof(line)) != 0) return -EFAULT;
    }
    return 0;
}

// ---- events ----

static int guest_interruptible(struct kvm_vcpu *vcpu) {
    return (*vmcb64(vcpu, VMCB_RFLAGS) & RFLAGS_IF) && !(*vmcb64(vcpu, VMCB_INT_STATE) & 1ULL);
}

// Whether userspace can inject the PIC's interrupt now, as ready_for_interrupt_injection
// tells it.
static int ready_for_injection(struct kvm_vcpu *vcpu) {
    return guest_interruptible(vcpu) && !vcpu->irq_pending && !(vcpu->reinject & EVENT_VALID) &&
        !vcpu->exception_pending && apic_accepts_pic(vcpu);
}

// The TPR the guest set through CR8, which writes V_TPR without exiting.
static void sync_guest_tpr(struct kvm_vcpu *vcpu) {
    uint8_t vtpr = (uint8_t)(*vmcb64(vcpu, VMCB_INT_CTL) & INT_CTL_V_TPR_MASK);
    if (vtpr != vcpu->vtpr_entered) apic_set(vcpu, APIC_TASKPRI, (uint32_t)vtpr << 4);
}

// What the guest was being given when it exited, to give again: an interrupt, an NMI or a
// fault. A software interrupt or INT3/INTO is not replayed; its instruction runs again.
static void note_interrupted_event(struct kvm_vcpu *vcpu) {
    // NMIs are blocked from one's delivery until its handler's IRET has completed: the
    // first exit after the IRET's address has been left behind.
    if (vcpu->awaiting_iret && *vmcb64(vcpu, VMCB_RIP) != vcpu->iret_rip) {
        vcpu->awaiting_iret = 0;
        vcpu->nmi_masked = 0;
    }
    uint64_t info = *vmcb64(vcpu, VMCB_EXIT_INT_INFO);
    vcpu->reinject = 0;
    if (!(info & EVENT_VALID)) return;
    uint64_t type = (info >> EVENT_TYPE_SHIFT) & 7ULL;
    unsigned vector = (unsigned)(info & 0xFFULL);
    if (type == EVENT_TYPE_SOFT) return;
    if (type == EVENT_TYPE_EXCEPTION && (vector == 3 || vector == 4)) return;
    vcpu->reinject = info;
}

static void prepare_events(struct kvm_vcpu *vcpu) {
    uint64_t inject = 0;
    int irqchip = vcpu->vm->irqchip, pic_vector;
    // A kick from now on is for something this pass has not seen.
    __atomic_store_n(&vcpu->kick, 0, __ATOMIC_SEQ_CST);
    if (vcpu->reinject & EVENT_VALID) {
        inject = vcpu->reinject;
        vcpu->reinject = 0;
    } else if (vcpu->exception_pending) {
        inject = vcpu->exception_vector | (EVENT_TYPE_EXCEPTION << EVENT_TYPE_SHIFT) | EVENT_VALID;
        if (vcpu->exception_has_error)
            inject |= EVENT_ERROR_VALID | ((uint64_t)vcpu->exception_error << 32);
        vcpu->exception_pending = 0;
    } else if (__atomic_load_n(&vcpu->nmi_pending, __ATOMIC_SEQ_CST) && !vcpu->nmi_masked) {
        inject = VECTOR_NMI | (EVENT_TYPE_NMI << EVENT_TYPE_SHIFT) | EVENT_VALID;
        __atomic_store_n(&vcpu->nmi_pending, 0, __ATOMIC_SEQ_CST);
        // shortcut: an NMI pending behind this one waits for an exit after the handler's
        // IRET; single-step over the IRET if that is too late for a guest.
        vcpu->nmi_masked = 1;
        *vmcb32(vcpu, VMCB_INTERCEPT_MISC1) |= INTERCEPT_IRET;
    } else if (vcpu->irq_pending && guest_interruptible(vcpu) && apic_accepts_pic(vcpu)) {
        inject = vcpu->irq_vector | (EVENT_TYPE_INTR << EVENT_TYPE_SHIFT) | EVENT_VALID;
        vcpu->irq_pending = 0;
    } else if (irqchip == IRQCHIP_FULL && guest_interruptible(vcpu) && apic_accepts_pic(vcpu) &&
               pic_pending(vcpu->vm) && (pic_vector = pic_take(vcpu->vm)) >= 0) {
        inject = (uint64_t)pic_vector | (EVENT_TYPE_INTR << EVENT_TYPE_SHIFT) | EVENT_VALID;
    } else if (irqchip && guest_interruptible(vcpu)) {
        int vector = apic_deliverable(vcpu);
        if (vector >= 0) {
            apic_clear_vector(vcpu, APIC_IRR, (unsigned)vector);
            apic_set_vector(vcpu, APIC_ISR, (unsigned)vector);
            inject = (uint64_t)vector | (EVENT_TYPE_INTR << EVENT_TYPE_SHIFT) | EVENT_VALID;
        }
    }
    *vmcb64(vcpu, VMCB_EVENT_INJ) = inject;

    // An exit as soon as the guest can take an interrupt: a virtual interrupt it never
    // sees, intercepted before delivery. For the APIC's, the processor itself compares
    // its priority with the guest's TPR.
    uint64_t ctl = *vmcb64(vcpu, VMCB_INT_CTL);
    ctl &= ~(INT_CTL_V_IRQ | (0xFULL << INT_CTL_V_PRIO_SHIFT) | INT_CTL_V_IGN_TPR);
    *vmcb32(vcpu, VMCB_INTERCEPT_MISC1) &= ~INTERCEPT_VINTR;
    if ((vcpu->irq_pending || vcpu->run->request_interrupt_window || pic_pending(vcpu->vm)) &&
        apic_accepts_pic(vcpu)) {
        ctl |= INT_CTL_V_IRQ | (0xFULL << INT_CTL_V_PRIO_SHIFT) | INT_CTL_V_IGN_TPR;
        *vmcb32(vcpu, VMCB_INTERCEPT_MISC1) |= INTERCEPT_VINTR;
    } else if (irqchip && apic_enabled(vcpu)) {
        int irr = apic_highest(vcpu, APIC_IRR), isr = apic_highest(vcpu, APIC_ISR);
        if (irr >= 0 && (irr >> 4) > (isr < 0 ? 0 : isr >> 4)) {
            ctl |= INT_CTL_V_IRQ | ((uint64_t)(irr >> 4) << INT_CTL_V_PRIO_SHIFT);
            *vmcb32(vcpu, VMCB_INTERCEPT_MISC1) |= INTERCEPT_VINTR;
        }
    }
    if (irqchip) {
        uint8_t vtpr = (uint8_t)((apic_get(vcpu, APIC_TASKPRI) >> 4) & 0xFU);
        ctl = (ctl & ~INT_CTL_V_TPR_MASK) | vtpr;
        vcpu->vtpr_entered = vtpr;
    }
    *vmcb64(vcpu, VMCB_INT_CTL) = ctl;
}

// ---- entering the guest ----

void kvm_svm_enter(uint64_t vmcb_pa, uint64_t *regs, uint64_t host_save_pa);

// The host's segment and system-call state is saved with VMSAVE around VMRUN, which
// itself saves and restores only what it must. Guest registers other than RAX and RSP
// are loaded and stored here.
__asm__(".text\n"
        ".type kvm_svm_enter, @function\n"
        "kvm_svm_enter:\n"
        "    push %rbp\n"
        "    push %rbx\n"
        "    push %r12\n"
        "    push %r13\n"
        "    push %r14\n"
        "    push %r15\n"
        "    push %rdx\n"
        "    push %rsi\n"
        "    push %rdi\n"
        "    mov %rdx, %rax\n"
        "    vmsave %rax\n"
        "    mov (%rsp), %rax\n"
        "    vmload %rax\n"
        "    mov 8(%rsi), %rcx\n"
        "    mov 16(%rsi), %rdx\n"
        "    mov 24(%rsi), %rbx\n"
        "    mov 40(%rsi), %rbp\n"
        "    mov 56(%rsi), %rdi\n"
        "    mov 64(%rsi), %r8\n"
        "    mov 72(%rsi), %r9\n"
        "    mov 80(%rsi), %r10\n"
        "    mov 88(%rsi), %r11\n"
        "    mov 96(%rsi), %r12\n"
        "    mov 104(%rsi), %r13\n"
        "    mov 112(%rsi), %r14\n"
        "    mov 120(%rsi), %r15\n"
        "    mov 48(%rsi), %rsi\n"
        "    vmrun %rax\n"
        "    vmsave %rax\n"
        "    push %rsi\n"
        "    mov 16(%rsp), %rsi\n"
        "    mov %rcx, 8(%rsi)\n"
        "    mov %rdx, 16(%rsi)\n"
        "    mov %rbx, 24(%rsi)\n"
        "    mov %rbp, 40(%rsi)\n"
        "    mov %rdi, 56(%rsi)\n"
        "    mov %r8, 64(%rsi)\n"
        "    mov %r9, 72(%rsi)\n"
        "    mov %r10, 80(%rsi)\n"
        "    mov %r11, 88(%rsi)\n"
        "    mov %r12, 96(%rsi)\n"
        "    mov %r13, 104(%rsi)\n"
        "    mov %r14, 112(%rsi)\n"
        "    mov %r15, 120(%rsi)\n"
        "    pop %rax\n"
        "    mov %rax, 48(%rsi)\n"
        "    mov 16(%rsp), %rax\n"
        "    vmload %rax\n"
        "    add $24, %rsp\n"
        "    pop %r15\n"
        "    pop %r14\n"
        "    pop %r13\n"
        "    pop %r12\n"
        "    pop %rbx\n"
        "    pop %rbp\n"
        "    ret\n"
        ".size kvm_svm_enter, .-kvm_svm_enter\n");

static void load_debug_registers(const uint64_t dr[4]) {
    __asm__ volatile("mov %0, %%dr0" : : "r"(dr[0]));
    __asm__ volatile("mov %0, %%dr1" : : "r"(dr[1]));
    __asm__ volatile("mov %0, %%dr2" : : "r"(dr[2]));
    __asm__ volatile("mov %0, %%dr3" : : "r"(dr[3]));
}

static void save_debug_registers(uint64_t dr[4]) {
    __asm__ volatile("mov %%dr0, %0" : "=r"(dr[0]));
    __asm__ volatile("mov %%dr1, %0" : "=r"(dr[1]));
    __asm__ volatile("mov %%dr2, %0" : "=r"(dr[2]));
    __asm__ volatile("mov %%dr3, %0" : "=r"(dr[3]));
}

// One VMRUN. Interrupts stay masked by GIF from before the FPU is switched until after it
// is switched back; a host interrupt while the guest runs makes it exit, and is taken
// when GIF is set again.
static int enter_guest(struct kvm_vcpu *vcpu) {
    uint64_t flags = cpu_irq_save();
    unsigned index = cpu_current()->index;
    if (index >= svm_cpu_slots) {
        cpu_irq_restore(flags);
        return -EIO;
    }
    struct svm_cpu *cpu = &svm_cpus[index];
    if (!cpu->enabled) {
        cpu_write_msr(MSR_EFER, cpu_read_msr(MSR_EFER) | EFER_SVME);
        cpu_write_msr(MSR_VM_HSAVE_PA, cpu->hsave);
        cpu->enabled = 1;
    }

    __atomic_store_n(&vcpu->cpu, index, __ATOMIC_SEQ_CST);
    __atomic_store_n(&vcpu->in_guest, 1, __ATOMIC_SEQ_CST);
    if (vcpu->vm->irqchip && __atomic_load_n(&vcpu->kick, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&vcpu->in_guest, 0, __ATOMIC_SEQ_CST);
        cpu_irq_restore(flags);
        return 1;
    }
    uint64_t timer = next_timer(vcpu);
    if (timer) timer_note_deadline(timer);
    uint64_t generation = __atomic_load_n(&vcpu->vm->generation, __ATOMIC_SEQ_CST);
    int flush = vcpu->flush || cpu->last_vcpu != vcpu->serial ||
        vcpu->entered_generation != generation;
    // The host never uses the debug registers; the guest's stay loaded until another vCPU
    // runs on this processor.
    if (cpu->last_vcpu != vcpu->serial) load_debug_registers(vcpu->dr);
    __atomic_store_n(&vcpu->entered_generation, generation, __ATOMIC_SEQ_CST);
    cpu->last_vcpu = vcpu->serial;
    vcpu->flush = 0;
    *(uint8_t *)(vcpu->vmcb + VMCB_TLB_CONTROL) = flush ? 1 : 0;
    *vmcb32(vcpu, VMCB_CLEAN) = 0;

    __asm__ volatile("clgi" ::: "memory");
    arch_fpu_save(vcpu->host_fpu);
    arch_fpu_restore(vcpu->guest_fpu);
    if (vcpu->xcr0 != host_xcr0) xsetbv0(vcpu->xcr0);
    __asm__ volatile("sti" ::: "memory");
    kvm_svm_enter(vcpu->vmcb_pa, vcpu->regs, cpu->host_save);
    __asm__ volatile("cli" ::: "memory");
    if (vcpu->xcr0 != host_xcr0) xsetbv0(host_xcr0);
    arch_fpu_save(vcpu->guest_fpu);
    arch_fpu_restore(vcpu->host_fpu);
    // shortcut: debug registers the guest wrote without enabling them in DR7 are lost when
    // another vCPU uses this processor; save them on every exit if a guest needs that.
    if (*vmcb64(vcpu, VMCB_DR7) & 0xFFULL) {
        save_debug_registers(vcpu->dr);
        __asm__ volatile("mov %0, %%dr7" : : "r"(0x400ULL));
    }
    __asm__ volatile("stgi" ::: "memory");
    __atomic_store_n(&vcpu->in_guest, 0, __ATOMIC_SEQ_CST);
    // The kernel runs with interrupts off, so the host interrupt that made the guest exit
    // would stay pending and end every VMRUN at once: it is taken here.
    if (*vmcb64(vcpu, VMCB_EXIT_CODE) == EXIT_INTR) __asm__ volatile("sti; nop; cli" ::: "memory");
    cpu_irq_restore(flags);
    return 0;
}

// ---- I/O event fds: a write to an address signals an eventfd instead of exiting ----

static int ioeventfd_signal(struct kvm_vm *vm, int pio, uint64_t address, unsigned length,
                            uint64_t value) {
    struct file *file = NULL;
    mutex_lock(&vm->lock);
    for (struct ioeventfd *item = vm->ioeventfds; item; item = item->next) {
        if (item->pio != pio || item->address != address) continue;
        if (item->length && item->length != length) continue;
        if (item->match && item->datamatch != value) continue;
        file = item->file;
        file_ref(file);
        break;
    }
    mutex_unlock(&vm->lock);
    if (!file) return 0;
    uint64_t one = 1;
    (void)file_write(file, sizeof(one), &one);
    (void)process_wake_io();
    file_unref(file);
    return 1;
}

// ---- MSRs ----

static int mtrr_index(uint32_t msr) {
    static const uint32_t fixed[11] = {0x250, 0x258, 0x259, 0x268, 0x269, 0x26A,
                                       0x26B, 0x26C, 0x26D, 0x26E, 0x26F};
    for (int index = 0; index < 11; index++)
        if (fixed[index] == msr) return index;
    return -1;
}

static int pat_valid(uint64_t value) {
    for (int byte = 0; byte < 8; byte++) {
        uint8_t type = (uint8_t)(value >> (byte * 8));
        if (type == 2 || type == 3 || type > 7) return 0;
    }
    return 1;
}

static uint64_t read_tsc(void) { return cpu_counter(); }

// Reads an MSR for the guest (host = 0) or for KVM_GET_MSRS. Returns -1 for one the guest
// cannot read, which becomes #GP.
static int msr_read(struct kvm_vcpu *vcpu, uint32_t msr, uint64_t *value, int host) {
    int fixed;
    switch (msr) {
    case 0x10:       *value = read_tsc() + *vmcb64(vcpu, VMCB_TSC_OFFSET); return 0;
    case 0x1B:       *value = vcpu->apic_base; return 0;
    case 0x8B:       *value = 0; return 0;
    case 0xFE:       *value = 0x508; return 0;
    case 0x174:      *value = *vmcb64(vcpu, VMCB_SYSENTER_CS); return 0;
    case 0x175:      *value = *vmcb64(vcpu, VMCB_SYSENTER_ESP); return 0;
    case 0x176:      *value = *vmcb64(vcpu, VMCB_SYSENTER_EIP); return 0;
    case 0x179:      *value = vcpu->mcg_cap; return 0;
    case 0x17A:      *value = vcpu->mcg_status; return 0;
    case 0x17B:
        if (!(vcpu->mcg_cap & MCG_CTL_P)) return -1;
        *value = vcpu->mcg_ctl;
        return 0;
    case 0x1A0:      *value = vcpu->misc_enable; return 0;
    case 0x277:      *value = *vmcb64(vcpu, VMCB_G_PAT); return 0;
    case 0x2FF:      *value = vcpu->mtrr_default; return 0;
    case MSR_EFER:   *value = *vmcb64(vcpu, VMCB_EFER) & ~EFER_SVME; return 0;
    case 0xC0000081: *value = *vmcb64(vcpu, VMCB_STAR); return 0;
    case 0xC0000082: *value = *vmcb64(vcpu, VMCB_LSTAR); return 0;
    case 0xC0000083: *value = *vmcb64(vcpu, VMCB_CSTAR); return 0;
    case 0xC0000084: *value = *vmcb64(vcpu, VMCB_SFMASK); return 0;
    case 0xC0000100: *value = *vmcb64(vcpu, VMCB_FS + 8); return 0;
    case 0xC0000101: *value = *vmcb64(vcpu, VMCB_GS + 8); return 0;
    case 0xC0000102: *value = *vmcb64(vcpu, VMCB_KGS); return 0;
    case 0xC0010015: *value = vcpu->hwcr; return 0;
    default:         break;
    }
    if (msr >= 0x200 && msr < 0x210) {
        *value = vcpu->mtrr_variable[msr - 0x200];
        return 0;
    }
    if ((fixed = mtrr_index(msr)) >= 0) {
        *value = vcpu->mtrr_fixed[fixed];
        return 0;
    }
    if (msr >= 0x400 && msr < 0x400 + 4 * (vcpu->mcg_cap & 0xFFULL)) {
        *value = vcpu->mce_banks[msr - 0x400];
        return 0;
    }
    if (host) {
        *value = 0;
        return 0;
    }
    return -1;
}

static int msr_write(struct kvm_vcpu *vcpu, uint32_t msr, uint64_t value, int host) {
    int fixed;
    switch (msr) {
    case 0x10: *vmcb64(vcpu, VMCB_TSC_OFFSET) = value - read_tsc(); return 0;
    case 0x1B:
        // There is no x2APIC.
        if (!host && (value & (0x2FFULL | APIC_BASE_EXTD | 0xFFFFFFF000000000ULL))) return -1;
        vcpu->apic_base = value;
        return 0;
    case 0x79: case 0x8B: return 0;
    case 0xFE: return host ? 0 : -1;
    case 0x174: *vmcb64(vcpu, VMCB_SYSENTER_CS) = value; return 0;
    case 0x175: *vmcb64(vcpu, VMCB_SYSENTER_ESP) = value; return 0;
    case 0x176: *vmcb64(vcpu, VMCB_SYSENTER_EIP) = value; return 0;
    case 0x179:
        if (!host) return -1;
        vcpu->mcg_cap = value;
        return 0;
    case 0x17A:
        if (!host && value) return -1;
        vcpu->mcg_status = value;
        return 0;
    case 0x17B:
        if (!(vcpu->mcg_cap & MCG_CTL_P)) return host ? 0 : -1;
        vcpu->mcg_ctl = value;
        return 0;
    case 0x1A0: vcpu->misc_enable = value; return 0;
    case 0x277:
        if (!pat_valid(value)) return -1;
        *vmcb64(vcpu, VMCB_G_PAT) = value;
        return 0;
    case 0x2FF:
        if (!host && (value & ~0xCFFULL)) return -1;
        vcpu->mtrr_default = value;
        return 0;
    case MSR_EFER: {
        uint64_t current = *vmcb64(vcpu, VMCB_EFER);
        if (!host) {
            if (value & ~(EFER_SCE | EFER_LME | EFER_LMA | EFER_NXE | EFER_FFXSR)) return -1;
            if ((*vmcb64(vcpu, VMCB_CR0) & CR0_PG) && ((value ^ current) & EFER_LME)) return -1;
            value = (value & ~EFER_LMA) | (current & EFER_LMA);
        }
        *vmcb64(vcpu, VMCB_EFER) = value | EFER_SVME;
        vcpu->flush = 1;
        return 0;
    }
    case 0xC0000081: *vmcb64(vcpu, VMCB_STAR) = value; return 0;
    case 0xC0000082: *vmcb64(vcpu, VMCB_LSTAR) = value; return 0;
    case 0xC0000083: *vmcb64(vcpu, VMCB_CSTAR) = value; return 0;
    case 0xC0000084: *vmcb64(vcpu, VMCB_SFMASK) = value; return 0;
    case 0xC0000100: *vmcb64(vcpu, VMCB_FS + 8) = value; return 0;
    case 0xC0000101: *vmcb64(vcpu, VMCB_GS + 8) = value; return 0;
    case 0xC0000102: *vmcb64(vcpu, VMCB_KGS) = value; return 0;
    case 0xC0010015: vcpu->hwcr = value; return 0;
    default:         break;
    }
    if (msr >= 0x200 && msr < 0x210) {
        vcpu->mtrr_variable[msr - 0x200] = value;
        return 0;
    }
    if ((fixed = mtrr_index(msr)) >= 0) {
        vcpu->mtrr_fixed[fixed] = value;
        return 0;
    }
    if (msr >= 0x400 && msr < 0x400 + 4 * (vcpu->mcg_cap & 0xFFULL)) {
        vcpu->mce_banks[msr - 0x400] = value;
        return 0;
    }
    if (host) {
        // Userspace restores what it saved, including MSRs this vCPU does not model.
        if (unknown_msr_reports < 16) {
            unknown_msr_reports++;
            kprintf("kvm: userspace set MSR 0x%x, which is not emulated; ignored\n", msr);
        }
        return 0;
    }
    return -1;
}

static const uint32_t saved_msrs[] = {
    0x10,       0x174,      0x175,      0x176,      0x17A,      0x1A0,
    0x277,      0xC0000081, 0xC0000082, 0xC0000083, 0xC0000084, 0xC0000102,
};

// ---- CPUID ----

static uint32_t xsave_size_for(uint64_t features) {
    uint32_t size = 576;
    for (unsigned component = 2; component < 63; component++) {
        if (!(features & (1ULL << component))) continue;
        uint32_t a, b, c, d;
        cpu_cpuid(0xD, component, &a, &b, &c, &d);
        if (a + b > size) size = a + b;
    }
    return size;
}

static void emulate_cpuid(struct kvm_vcpu *vcpu) {
    uint32_t leaf = (uint32_t)get_reg(vcpu, R_AX), subleaf = (uint32_t)get_reg(vcpu, R_CX);
    uint32_t a = 0, b = 0, c = 0, d = 0;
    for (uint32_t index = 0; index < vcpu->cpuid_count; index++) {
        const struct kvm_cpuid_entry2 *entry = &vcpu->cpuid[index];
        if (entry->function != leaf) continue;
        if ((entry->flags & KVM_CPUID_FLAG_SIGNIFCANT_INDEX) && entry->index != subleaf) continue;
        a = entry->eax;
        b = entry->ebx;
        c = entry->ecx;
        d = entry->edx;
        break;
    }
    if (leaf == 1) {
        c &= ~(1U << 27);
        if (*vmcb64(vcpu, VMCB_CR4) & CR4_OSXSAVE) c |= 1U << 27;
        if (!(vcpu->apic_base & (1ULL << 11))) d &= ~(1U << 9);
    }
    if (leaf == 0xD && (subleaf == 0 || subleaf == 1)) b = xsave_size_for(vcpu->xcr0);
    put_reg(vcpu, R_AX, a);
    put_reg(vcpu, R_BX, b);
    put_reg(vcpu, R_CX, c);
    put_reg(vcpu, R_DX, d);
    skip_instruction(vcpu);
}

static int add_cpuid(struct kvm_cpuid_entry2 *entries, uint32_t *count, uint32_t capacity,
                     uint32_t leaf, uint32_t subleaf, int indexed) {
    if (*count >= capacity) return -E2BIG;
    struct kvm_cpuid_entry2 *entry = &entries[(*count)++];
    memset(entry, 0, sizeof(*entry));
    entry->function = leaf;
    entry->index = subleaf;
    entry->flags = indexed ? KVM_CPUID_FLAG_SIGNIFCANT_INDEX : 0;
    cpu_cpuid(leaf, subleaf, &entry->eax, &entry->ebx, &entry->ecx, &entry->edx);

    // Features this vCPU does not give the guest: virtualization itself, monitoring and
    // performance counters, power management, the x2APIC and TSC deadline timer of an
    // in-kernel APIC, speculation controls, and anything needing MSRs it lacks.
    switch (leaf) {
    case 1:
        entry->ecx &= ~((1U << 3) | (1U << 4) | (1U << 5) | (1U << 6) | (1U << 7) | (1U << 8) |
                        (1U << 14) | (1U << 15) | (1U << 18) | (1U << 21) | (1U << 24) |
                        (1U << 27));
        entry->ecx |= 1U << 31;
        entry->edx &= ~((1U << 21) | (1U << 22) | (1U << 29) | (1U << 31));
        break;
    case 5: case 6: case 0xA: case 0xF: case 0x10: case 0x12: case 0x14:
        entry->eax = entry->ebx = entry->ecx = entry->edx = 0;
        break;
    case 7:
        if (subleaf == 0) {
            entry->eax = 0;
            entry->ebx &= (1U << 0) | (1U << 3) | (1U << 5) | (1U << 7) | (1U << 8) | (1U << 9) |
                (1U << 18) | (1U << 19) | (1U << 20) | (1U << 23) | (1U << 24) | (1U << 29);
            entry->ecx &= (1U << 8) | (1U << 9) | (1U << 10);
            entry->edx = 0;
        } else {
            entry->eax = entry->ebx = entry->ecx = entry->edx = 0;
        }
        break;
    case 0xD:
        if (subleaf == 0) {
            entry->eax = (uint32_t)host_xcr0;
            entry->edx = (uint32_t)(host_xcr0 >> 32);
            entry->ebx = xsave_size_for(host_xcr0);
        } else if (subleaf == 1) {
            entry->eax &= 0x7U;
            entry->ebx = entry->ecx = entry->edx = 0;
        }
        break;
    case 0x80000001:
        entry->ecx &= ~((1U << 2) | (1U << 9) | (1U << 10) | (1U << 12) | (1U << 13) |
                        (1U << 15) | (1U << 19) | (1U << 23) | (1U << 24) | (1U << 26) |
                        (1U << 27) | (1U << 28) | (1U << 29));
        entry->edx &= ~(1U << 27);
        break;
    case 0x80000007:
        entry->eax = entry->ebx = entry->ecx = 0;
        entry->edx &= 1U << 8;
        break;
    case 0x80000008:
        entry->ebx &= (1U << 0) | (1U << 2);
        break;
    case 0x8000000A: case 0x8000001B: case 0x8000001F: case 0x80000020: case 0x80000022:
    case 0x80000026:
        entry->eax = entry->ebx = entry->ecx = entry->edx = 0;
        break;
    case 0x80000021:
        entry->eax &= (1U << 0) | (1U << 2) | (1U << 6);
        entry->ebx = entry->ecx = entry->edx = 0;
        break;
    default:
        break;
    }
    return 0;
}

static int supported_cpuid(struct kvm_cpuid_entry2 *entries, uint32_t capacity,
                           uint32_t *count_out) {
    uint32_t count = 0, a, b, c, d;
    int status = 0;
    cpu_cpuid(0, 0, &a, &b, &c, &d);
    uint32_t max_basic = a > 0x1F ? 0x1F : a;
    for (uint32_t leaf = 0; leaf <= max_basic && !status; leaf++) {
        if (leaf == 4 || leaf == 0xB || leaf == 0x1F) {
            for (uint32_t sub = 0; sub < 64 && !status; sub++) {
                status = add_cpuid(entries, &count, capacity, leaf, sub, 1);
                if (status) break;
                const struct kvm_cpuid_entry2 *last = &entries[count - 1];
                if (leaf == 4 ? !(last->eax & 0x1FU) : !(last->ecx & 0xFF00U)) break;
            }
        } else if (leaf == 7) {
            status = add_cpuid(entries, &count, capacity, 7, 0, 1);
        } else if (leaf == 0xD) {
            status = add_cpuid(entries, &count, capacity, 0xD, 0, 1);
            if (!status) status = add_cpuid(entries, &count, capacity, 0xD, 1, 1);
            for (uint32_t sub = 2; sub < 63 && !status; sub++)
                if (host_xcr0 & (1ULL << sub))
                    status = add_cpuid(entries, &count, capacity, 0xD, sub, 1);
        } else {
            status = add_cpuid(entries, &count, capacity, leaf, 0, 0);
        }
    }
    cpu_cpuid(0x80000000U, 0, &a, &b, &c, &d);
    uint32_t max_extended = a > 0x80000021U ? 0x80000021U : a;
    for (uint32_t leaf = 0x80000000U; leaf <= max_extended && !status; leaf++) {
        if (leaf == 0x8000001DU) {
            for (uint32_t sub = 0; sub < 64 && !status; sub++) {
                status = add_cpuid(entries, &count, capacity, leaf, sub, 1);
                if (status || !(entries[count - 1].eax & 0x1FU)) break;
            }
        } else {
            status = add_cpuid(entries, &count, capacity, leaf, 0, 0);
        }
    }
    *count_out = count;
    return status;
}

// ---- exits ----

static void exit_internal_error(struct kvm_vcpu *vcpu, uint32_t suberror) {
    struct kvm_run *run = vcpu->run;
    run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
    run->internal.suberror = suberror;
    run->internal.ndata = 4;
    run->internal.data[0] = *vmcb64(vcpu, VMCB_EXIT_CODE);
    run->internal.data[1] = *vmcb64(vcpu, VMCB_EXIT_INFO1);
    run->internal.data[2] = *vmcb64(vcpu, VMCB_EXIT_INFO2);
    run->internal.data[3] = *vmcb64(vcpu, VMCB_RIP);
}

static int exit_io(struct kvm_vcpu *vcpu) {
    struct kvm_run *run = vcpu->run;
    uint64_t info = *vmcb64(vcpu, VMCB_EXIT_INFO1);
    uint64_t next_rip = *vmcb64(vcpu, VMCB_EXIT_INFO2);
    uint16_t port = (uint16_t)(info >> 16);
    int in = (int)(info & 1U), string = (info & 4U) != 0, rep = (info & 8U) != 0;
    unsigned size = (info & 0x10U) ? 1 : (info & 0x20U) ? 2 : 4;
    unsigned address_size = (info & 0x80U) ? 2 : (info & 0x100U) ? 4 : 8;
    unsigned count = 1;

    if (!string) {
        uint8_t byte = (uint8_t)get_reg(vcpu, R_AX);
        if (chips_io(vcpu, port, in, size, &byte)) {
            if (in) write_reg_sized(vcpu, R_AX, 0, 1, byte);
            advance_rip(vcpu, next_rip);
            return 0;
        }
        if (!in) {
            uint64_t value = read_reg_sized(vcpu, R_AX, 0, size);
            advance_rip(vcpu, next_rip);
            if (ioeventfd_signal(vcpu->vm, 1, port, size, value)) return 0;
            memcpy(vcpu->io_data, &value, size);
        } else {
            advance_rip(vcpu, next_rip);
            vcpu->done = (struct completion){.kind = DONE_PIO_IN, .size = size, .count = 1};
        }
    } else {
        uint64_t mask = address_mask(address_size);
        if (rep) {
            uint64_t remaining = get_reg(vcpu, R_CX) & mask;
            if (!remaining) {
                advance_rip(vcpu, next_rip);
                return 0;
            }
            count = remaining > 4096U / size ? 4096U / size : (unsigned)remaining;
        }
        int backwards = (*vmcb64(vcpu, VMCB_RFLAGS) & RFLAGS_DF) != 0;
        int index_reg = in ? R_DI : R_SI;
        unsigned segment = in ? 0 : (unsigned)((info >> 10) & 7U);
        uint64_t linear = segment_base(vcpu, segment) + (get_reg(vcpu, index_reg) & mask);
        if (!long_mode_code(vcpu)) linear &= 0xFFFFFFFFULL;
        // Batched only forwards and within one page of the guest's memory.
        unsigned in_page = (4096U - (unsigned)(linear & 0xFFFULL)) / size;
        if (backwards || !in_page) count = 1;
        else if (count > in_page) count = in_page;
        if (!in) {
            if (guest_linear_access(vcpu, linear, vcpu->io_data, (size_t)count * size, 0) != 0)
                return -EFAULT;
        } else {
            vcpu->done = (struct completion){
                .kind = DONE_INS, .size = size, .count = count, .linear = linear};
        }
        int64_t step = (int64_t)count * size;
        add_reg_masked(vcpu, index_reg, address_size, backwards ? -step : step);
        if (rep) add_reg_masked(vcpu, R_CX, address_size, -(int64_t)count);
        if (!rep || !(get_reg(vcpu, R_CX) & mask)) advance_rip(vcpu, next_rip);
    }
    run->exit_reason = KVM_EXIT_IO;
    run->io.direction = in ? KVM_EXIT_IO_IN : KVM_EXIT_IO_OUT;
    run->io.size = (uint8_t)size;
    run->io.port = port;
    run->io.count = count;
    run->io.data_offset = KVM_IO_DATA_OFFSET;
    return 1;
}

// An access to the APIC page. Registers sit every 16 bytes; a write must be a whole one,
// a read can be part of one.
static int apic_mmio(struct kvm_vcpu *vcpu, uint64_t gpa, int write, uint64_t value,
                     unsigned size) {
    unsigned offset = (unsigned)(gpa & 0xFFFULL), aligned = offset & 0xFF0U;
    unsigned within = offset & 0xFU;
    if (write) {
        if (size != 4 || within) return 0;
        int vector = apic_write(vcpu, aligned, (uint32_t)value);
        if (vector < 0) return 0;
        if (vcpu->vm->irqchip == IRQCHIP_FULL) {
            mutex_lock(&vcpu->vm->irq_lock);
            ioapic_eoi(vcpu->vm, (unsigned)vector);
            mutex_unlock(&vcpu->vm->irq_lock);
            return 0;
        }
        vcpu->done.kind = DONE_NONE;
        vcpu->run->exit_reason = KVM_EXIT_IOAPIC_EOI;
        vcpu->run->eoi.vector = (uint8_t)vector;
        return 1;
    }
    uint64_t data = within + size <= 4 ? apic_read(vcpu, aligned) >> (within * 8U) : 0;
    memcpy(vcpu->run->mmio.data, &data, sizeof(data));
    return complete_userspace(vcpu) == 0 ? 0 : -EFAULT;
}

static int exit_mmio(struct kvm_vcpu *vcpu, uint64_t gpa) {
    struct kvm_run *run = vcpu->run;
    uint8_t bytes[15];
    unsigned available;
    struct instruction insn;
    if (fetch_instruction(vcpu, bytes, &available) != 0 ||
        decode(vcpu, bytes, available, &insn) != 0) {
        kprintf("kvm: cannot emulate the MMIO access at %p, rip %p: %x %x %x %x\n",
                (void *)(uintptr_t)gpa, (void *)(uintptr_t)*vmcb64(vcpu, VMCB_RIP), bytes[0],
                bytes[1], bytes[2], bytes[3]);
        exit_internal_error(vcpu, KVM_INTERNAL_ERROR_EMULATION);
        return 1;
    }
    uint64_t rip = *vmcb64(vcpu, VMCB_RIP);
    uint64_t next_rip = rip + insn.length;
    if (!long_mode_code(vcpu)) next_rip &= default_size(vcpu) == 2 ? 0xFFFFULL : 0xFFFFFFFFULL;
    int write = 0;
    uint64_t value = 0;

    if (insn.op == OP_MOVS || insn.op == OP_STOS || insn.op == OP_LODS) {
        uint64_t mask = address_mask(insn.address_size);
        int backwards = (*vmcb64(vcpu, VMCB_RFLAGS) & RFLAGS_DF) != 0;
        int64_t step = backwards ? -(int64_t)insn.size : (int64_t)insn.size;
        unsigned source_segment = insn.segment >= 0 ? (unsigned)insn.segment : 3;
        uint64_t source = segment_base(vcpu, source_segment) + (get_reg(vcpu, R_SI) & mask);
        uint64_t dest = segment_base(vcpu, 0) + (get_reg(vcpu, R_DI) & mask);
        if (!long_mode_code(vcpu)) {
            source &= 0xFFFFFFFFULL;
            dest &= 0xFFFFFFFFULL;
        }
        if (insn.op == OP_STOS) {
            write = 1;
            value = read_reg_sized(vcpu, R_AX, 0, insn.size);
            add_reg_masked(vcpu, R_DI, insn.address_size, step);
        } else if (insn.op == OP_LODS) {
            vcpu->done = (struct completion){.kind = DONE_MMIO_REG, .size = insn.size,
                                             .dest_size = insn.size, .reg = R_AX};
            add_reg_masked(vcpu, R_SI, insn.address_size, step);
        } else {
            uint64_t source_gpa, dest_gpa;
            int source_mmio = guest_translate(vcpu, source, &source_gpa) != 0 ||
                !slot_for(vcpu->vm, source_gpa);
            int dest_mmio = guest_translate(vcpu, dest, &dest_gpa) != 0 ||
                !slot_for(vcpu->vm, dest_gpa);
            if (source_mmio == dest_mmio) {
                exit_internal_error(vcpu, KVM_INTERNAL_ERROR_EMULATION);
                return 1;
            }
            if (dest_mmio) {
                write = 1;
                if (guest_linear_access(vcpu, source, &value, insn.size, 0) != 0) return -EFAULT;
                gpa = dest_gpa;
            } else {
                vcpu->done = (struct completion){
                    .kind = DONE_MMIO_MEMORY, .size = insn.size, .linear = dest};
                gpa = source_gpa;
            }
            add_reg_masked(vcpu, R_SI, insn.address_size, step);
            add_reg_masked(vcpu, R_DI, insn.address_size, step);
        }
        if (insn.rep) {
            add_reg_masked(vcpu, R_CX, insn.address_size, -1);
            if (get_reg(vcpu, R_CX) & mask) next_rip = rip;
        }
    } else if (insn.op == OP_LOAD) {
        vcpu->done = (struct completion){.kind = DONE_MMIO_REG,
                                         .size = insn.size,
                                         .dest_size = insn.dest_size,
                                         .reg = insn.reg,
                                         .high_byte = insn.high_byte,
                                         .sign_extend = insn.sign_extend};
    } else {
        write = 1;
        value = insn.op == OP_STORE_IMM ? insn.immediate
                                        : read_reg_sized(vcpu, insn.reg, insn.high_byte, insn.size);
    }
    if (next_rip != rip) advance_rip(vcpu, next_rip);
    if (apic_page(vcpu, gpa)) return apic_mmio(vcpu, gpa, write, value, insn.size);
    if (vcpu->vm->irqchip == IRQCHIP_FULL && (gpa & ~0xFFFULL) == IOAPIC_BASE)
        return ioapic_mmio(vcpu, gpa, write, value);
    if (write && ioeventfd_signal(vcpu->vm, 0, gpa, insn.size, value)) return 0;
    run->exit_reason = KVM_EXIT_MMIO;
    run->mmio.phys_addr = gpa;
    run->mmio.len = insn.size;
    run->mmio.is_write = (uint8_t)write;
    memset(run->mmio.data, 0, sizeof(run->mmio.data));
    if (write) memcpy(run->mmio.data, &value, insn.size);
    return 1;
}

static int exit_npf(struct kvm_vcpu *vcpu) {
    uint64_t gpa = *vmcb64(vcpu, VMCB_EXIT_INFO2);
    struct kvm_vm *vm = vcpu->vm;
    if (gpa >= GPA_LIMIT) {
        exit_internal_error(vcpu, KVM_INTERNAL_ERROR_EMULATION);
        return 1;
    }
    mutex_lock(&vm->lock);
    int status = npt_fault(vm, gpa);
    mutex_unlock(&vm->lock);
    if (status < 0) return status;
    if (status == 0) return 0;
    if (vcpu->reinject & EVENT_VALID) {
        // An event's delivery touched device memory: nothing this emulates.
        exit_internal_error(vcpu, KVM_INTERNAL_ERROR_SIMUL_EX);
        return 1;
    }
    return exit_mmio(vcpu, gpa);
}

static int exit_msr(struct kvm_vcpu *vcpu) {
    uint32_t msr = (uint32_t)get_reg(vcpu, R_CX);
    if (*vmcb64(vcpu, VMCB_EXIT_INFO1)) {
        uint64_t value = (get_reg(vcpu, R_AX) & 0xFFFFFFFFULL) | (get_reg(vcpu, R_DX) << 32);
        if (msr_write(vcpu, msr, value, 0) != 0) {
            queue_exception(vcpu, VECTOR_GP, 1, 0);
            return 0;
        }
    } else {
        uint64_t value;
        if (msr_read(vcpu, msr, &value, 0) != 0) {
            queue_exception(vcpu, VECTOR_GP, 1, 0);
            return 0;
        }
        put_reg(vcpu, R_AX, value & 0xFFFFFFFFULL);
        put_reg(vcpu, R_DX, value >> 32);
    }
    skip_instruction(vcpu);
    return 0;
}

static int xcr0_valid(uint64_t value) {
    if (!(value & 1ULL) || (value & ~host_xcr0)) return 0;
    if ((value & 4ULL) && !(value & 2ULL)) return 0;
    return 1;
}

static int exit_xsetbv(struct kvm_vcpu *vcpu) {
    uint64_t value = (get_reg(vcpu, R_AX) & 0xFFFFFFFFULL) | (get_reg(vcpu, R_DX) << 32);
    if ((uint32_t)get_reg(vcpu, R_CX) != 0 || *(uint8_t *)(vcpu->vmcb + VMCB_CPL) != 0 ||
        !xcr0_valid(value)) {
        queue_exception(vcpu, VECTOR_GP, 1, 0);
        return 0;
    }
    vcpu->xcr0 = value;
    skip_instruction(vcpu);
    return 0;
}

static void vmcb_init_control(struct kvm_vcpu *vcpu);

// Returns 0 to enter the guest again, 1 to return to userspace, or an error.
static int handle_exit(struct kvm_vcpu *vcpu) {
    uint64_t code = *vmcb64(vcpu, VMCB_EXIT_CODE);
    struct kvm_run *run = vcpu->run;
    note_interrupted_event(vcpu);
    switch (code) {
    case EXIT_INTR: case EXIT_NMI: case EXIT_SMI: case EXIT_INIT:
        return 0;
    case EXIT_VINTR:
        *vmcb64(vcpu, VMCB_INT_CTL) &= ~INT_CTL_V_IRQ;
        *vmcb32(vcpu, VMCB_INTERCEPT_MISC1) &= ~INTERCEPT_VINTR;
        return 0;
    case EXIT_CPUID:
        emulate_cpuid(vcpu);
        return 0;
    case EXIT_HLT:
        skip_instruction(vcpu);
        if (vcpu->vm->irqchip) {
            vcpu->mp_state = KVM_MP_STATE_HALTED;
            return 0;
        }
        run->exit_reason = KVM_EXIT_HLT;
        return 1;
    case EXIT_IRET:
        vcpu->awaiting_iret = 1;
        vcpu->iret_rip = *vmcb64(vcpu, VMCB_RIP);
        *vmcb32(vcpu, VMCB_INTERCEPT_MISC1) &= ~INTERCEPT_IRET;
        return 0;
    case EXIT_IOIO:     return exit_io(vcpu);
    case EXIT_MSR:      return exit_msr(vcpu);
    case EXIT_NPF:      return exit_npf(vcpu);
    case EXIT_XSETBV:   return exit_xsetbv(vcpu);
    case EXIT_INVD:
        skip_instruction(vcpu);
        return 0;
    case EXIT_EXCEPTION_DB:
        queue_exception(vcpu, VECTOR_DB, 0, 0);
        return 0;
    case EXIT_EXCEPTION_AC:
        queue_exception(vcpu, VECTOR_AC, 1, (uint32_t)*vmcb64(vcpu, VMCB_EXIT_INFO1));
        return 0;
    case EXIT_VMRUN: case EXIT_VMMCALL: case EXIT_VMLOAD: case EXIT_VMSAVE: case EXIT_STGI:
    case EXIT_CLGI: case EXIT_SKINIT: case EXIT_INVLPGA: case EXIT_MONITOR: case EXIT_MWAIT:
    case EXIT_MWAIT_COND: case EXIT_RDPRU:
        queue_exception(vcpu, VECTOR_UD, 0, 0);
        return 0;
    case EXIT_SHUTDOWN:
        // The VMCB is undefined after a shutdown; userspace resets the vCPU's state.
        vmcb_init_control(vcpu);
        run->exit_reason = KVM_EXIT_SHUTDOWN;
        return 1;
    case EXIT_TASK_SWITCH:
        exit_internal_error(vcpu, KVM_INTERNAL_ERROR_EMULATION);
        return 1;
    case EXIT_INVALID:
        run->exit_reason = KVM_EXIT_FAIL_ENTRY;
        run->fail_entry.hardware_entry_failure_reason = code;
        run->fail_entry.cpu = cpu_current()->index;
        kprintf("kvm: VMRUN refused the guest state: cr0 %p cr4 %p efer %p rip %p\n",
                (void *)(uintptr_t)*vmcb64(vcpu, VMCB_CR0),
                (void *)(uintptr_t)*vmcb64(vcpu, VMCB_CR4),
                (void *)(uintptr_t)*vmcb64(vcpu, VMCB_EFER),
                (void *)(uintptr_t)*vmcb64(vcpu, VMCB_RIP));
        return 1;
    default:
        run->exit_reason = KVM_EXIT_UNKNOWN;
        run->hw.hardware_exit_reason = code;
        return 1;
    }
}

// Finishes what userspace was asked to do: the data of an IN, INS or MMIO load.
static int complete_userspace(struct kvm_vcpu *vcpu) {
    struct completion done = vcpu->done;
    struct kvm_run *run = vcpu->run;
    vcpu->done.kind = DONE_NONE;
    switch (done.kind) {
    case DONE_PIO_IN: {
        uint64_t value = 0;
        memcpy(&value, vcpu->io_data, done.size);
        write_reg_sized(vcpu, R_AX, 0, done.size, value);
        return 0;
    }
    case DONE_INS:
        return guest_linear_access(vcpu, done.linear, vcpu->io_data,
                                   (size_t)done.count * done.size, 1);
    case DONE_MMIO_REG: {
        uint64_t value = 0;
        memcpy(&value, run->mmio.data, done.size);
        if (done.sign_extend) {
            unsigned shift = 64U - done.size * 8U;
            value = (uint64_t)((int64_t)(value << shift) >> shift);
        }
        write_reg_sized(vcpu, done.reg, done.high_byte, done.dest_size, value);
        return 0;
    }
    case DONE_MMIO_MEMORY:
        return guest_linear_access(vcpu, done.linear, run->mmio.data, done.size, 1);
    default:
        return 0;
    }
}

static void fill_run_state(struct kvm_vcpu *vcpu) {
    struct kvm_run *run = vcpu->run;
    run->if_flag = (*vmcb64(vcpu, VMCB_RFLAGS) & RFLAGS_IF) ? 1 : 0;
    run->ready_for_interrupt_injection = (uint8_t)ready_for_injection(vcpu);
    run->cr8 = vcpu->vm->irqchip ? (apic_get(vcpu, APIC_TASKPRI) >> 4) & 0xFU
                                 : *vmcb64(vcpu, VMCB_INT_CTL) & INT_CTL_V_TPR_MASK;
    run->apic_base = vcpu->apic_base;
    run->flags = 0;
}

static int64_t vcpu_run(struct kvm_vcpu *vcpu) {
    struct kvm_run *run = vcpu->run;
    struct process *process = process_current();
    if (!process || process->cr3 != vcpu->vm->cr3) return -EIO;
    if (complete_userspace(vcpu) != 0) return -EFAULT;
    if (run->immediate_exit) {
        run->exit_reason = KVM_EXIT_INTR;
        fill_run_state(vcpu);
        return -EINTR;
    }
    int irqchip = vcpu->vm->irqchip;
    if (!irqchip) {
        uint64_t *ctl = vmcb64(vcpu, VMCB_INT_CTL);
        *ctl = (*ctl & ~INT_CTL_V_TPR_MASK) | (run->cr8 & INT_CTL_V_TPR_MASK);
    }
    for (;;) {
        if (process_signal_interrupts_wait()) {
            run->exit_reason = KVM_EXIT_INTR;
            fill_run_state(vcpu);
            return -EINTR;
        }
        process_preempt_point();
        if (irqchip && vcpu_block(vcpu) != 0) {
            run->exit_reason = KVM_EXIT_INTR;
            fill_run_state(vcpu);
            return -EINTR;
        }
        prepare_events(vcpu);
        int status = enter_guest(vcpu);
        if (status < 0) return status;
        if (status > 0) continue;
        if (irqchip) sync_guest_tpr(vcpu);
        status = handle_exit(vcpu);
        if (status < 0) {
            fill_run_state(vcpu);
            return status;
        }
        if (status > 0) break;
        if (run->request_interrupt_window && ready_for_injection(vcpu)) {
            run->exit_reason = KVM_EXIT_IRQ_WINDOW_OPEN;
            break;
        }
    }
    fill_run_state(vcpu);
    return 0;
}

// ---- vCPU state ----

static void vmcb_segment(struct kvm_vcpu *vcpu, unsigned offset, uint16_t selector,
                         uint16_t attributes, uint32_t limit, uint64_t base) {
    *(uint16_t *)(vcpu->vmcb + offset) = selector;
    *(uint16_t *)(vcpu->vmcb + offset + 2) = attributes;
    *(uint32_t *)(vcpu->vmcb + offset + 4) = limit;
    *(uint64_t *)(vcpu->vmcb + offset + 8) = base;
}

static void vmcb_init_control(struct kvm_vcpu *vcpu) {
    memset(vcpu->vmcb, 0, 0x400);
    *vmcb32(vcpu, VMCB_INTERCEPT_EXCEPTIONS) = (1U << VECTOR_DB) | (1U << VECTOR_AC);
    *vmcb32(vcpu, VMCB_INTERCEPT_MISC1) = INTERCEPT_INTR | INTERCEPT_NMI | INTERCEPT_SMI |
        INTERCEPT_INIT | INTERCEPT_CPUID | INTERCEPT_INVD | INTERCEPT_HLT | INTERCEPT_INVLPGA |
        INTERCEPT_IOIO | INTERCEPT_MSR | INTERCEPT_TASK_SWITCH | INTERCEPT_SHUTDOWN;
    *vmcb32(vcpu, VMCB_INTERCEPT_MISC2) = INTERCEPT_VMRUN | INTERCEPT_VMMCALL | INTERCEPT_VMLOAD |
        INTERCEPT_VMSAVE | INTERCEPT_STGI | INTERCEPT_CLGI | INTERCEPT_SKINIT | INTERCEPT_MONITOR |
        INTERCEPT_MWAIT | INTERCEPT_MWAIT_COND | INTERCEPT_XSETBV | INTERCEPT_RDPRU;
    *vmcb64(vcpu, VMCB_IOPM_BASE) = iopm_pa;
    *vmcb64(vcpu, VMCB_MSRPM_BASE) = msrpm_pa;
    *vmcb32(vcpu, VMCB_ASID) = 1;
    *vmcb64(vcpu, VMCB_INT_CTL) = INT_CTL_V_INTR_MASK;
    *vmcb64(vcpu, VMCB_NP_ENABLE) = 1;
    *vmcb64(vcpu, VMCB_NCR3) = vcpu->vm->npt_root;
    vcpu->flush = 1;
}

// The state of a processor after RESET, which userspace normally overwrites.
static void vmcb_init_state(struct kvm_vcpu *vcpu) {
    memset(vcpu->vmcb + 0x400, 0, 0xC00);
    vmcb_segment(vcpu, VMCB_CS, 0xF000, 0x9B, 0xFFFF, 0xFFFF0000ULL);
    vmcb_segment(vcpu, VMCB_DS, 0, 0x93, 0xFFFF, 0);
    vmcb_segment(vcpu, VMCB_ES, 0, 0x93, 0xFFFF, 0);
    vmcb_segment(vcpu, VMCB_SS, 0, 0x93, 0xFFFF, 0);
    vmcb_segment(vcpu, VMCB_FS, 0, 0x93, 0xFFFF, 0);
    vmcb_segment(vcpu, VMCB_GS, 0, 0x93, 0xFFFF, 0);
    vmcb_segment(vcpu, VMCB_GDTR, 0, 0, 0xFFFF, 0);
    vmcb_segment(vcpu, VMCB_IDTR, 0, 0, 0xFFFF, 0);
    vmcb_segment(vcpu, VMCB_LDTR, 0, 0x82, 0xFFFF, 0);
    vmcb_segment(vcpu, VMCB_TR, 0, 0x8B, 0xFFFF, 0);
    *vmcb64(vcpu, VMCB_CR0) = 0x60000010ULL;
    *vmcb64(vcpu, VMCB_EFER) = EFER_SVME;
    *vmcb64(vcpu, VMCB_RFLAGS) = 2;
    *vmcb64(vcpu, VMCB_RIP) = 0xFFF0;
    *vmcb64(vcpu, VMCB_DR6) = 0xFFFF0FF0ULL;
    *vmcb64(vcpu, VMCB_DR7) = 0x400;
    *vmcb64(vcpu, VMCB_G_PAT) = 0x0007040600070406ULL;
}

static const unsigned sreg_offsets[8] = {VMCB_CS, VMCB_DS, VMCB_ES, VMCB_FS,
                                         VMCB_GS, VMCB_SS, VMCB_TR, VMCB_LDTR};

static void segment_get(struct kvm_vcpu *vcpu, unsigned offset, struct kvm_segment *segment) {
    uint16_t attributes = *(uint16_t *)(vcpu->vmcb + offset + 2);
    memset(segment, 0, sizeof(*segment));
    segment->selector = *(uint16_t *)(vcpu->vmcb + offset);
    segment->limit = *(uint32_t *)(vcpu->vmcb + offset + 4);
    segment->base = *(uint64_t *)(vcpu->vmcb + offset + 8);
    segment->type = (uint8_t)SEG_TYPE(attributes);
    segment->s = (attributes & SEG_S) != 0;
    segment->dpl = (attributes >> 5) & 3U;
    segment->present = (attributes & SEG_P) != 0;
    segment->avl = (attributes & SEG_AVL) != 0;
    segment->l = (attributes & SEG_L) != 0;
    segment->db = (attributes & SEG_DB) != 0;
    segment->g = (attributes & SEG_G) != 0;
    segment->unusable = !segment->present;
    // As Linux reports them: a busy TSS, an accessed data segment, SS's DPL from the CPL.
    if (offset == VMCB_TR) segment->type |= 2;
    else if (offset != VMCB_CS && offset != VMCB_LDTR && !segment->unusable) segment->type |= 1;
    if (offset == VMCB_SS) {
        if (segment->unusable) segment->db = 0;
        segment->dpl = *(uint8_t *)(vcpu->vmcb + VMCB_CPL) & 3U;
    }
}

static void segment_set(struct kvm_vcpu *vcpu, unsigned offset, const struct kvm_segment *segment) {
    uint16_t attributes = 0;
    if (!segment->unusable)
        attributes = (uint16_t)((segment->type & 0xFU) | (segment->s ? SEG_S : 0) |
                                ((segment->dpl & 3U) << 5) | (segment->present ? SEG_P : 0) |
                                (segment->avl ? SEG_AVL : 0) | (segment->l ? SEG_L : 0) |
                                (segment->db ? SEG_DB : 0) | (segment->g ? SEG_G : 0));
    vmcb_segment(vcpu, offset, segment->selector, attributes, segment->limit, segment->base);
    if (offset == VMCB_SS) *(uint8_t *)(vcpu->vmcb + VMCB_CPL) = segment->dpl & 3U;
}

static int64_t get_sregs(struct kvm_vcpu *vcpu, uint64_t argument) {
    struct kvm_sregs sregs;
    memset(&sregs, 0, sizeof(sregs));
    struct kvm_segment *segments[8] = {&sregs.cs, &sregs.ds, &sregs.es, &sregs.fs,
                                       &sregs.gs, &sregs.ss, &sregs.tr, &sregs.ldt};
    for (int index = 0; index < 8; index++) segment_get(vcpu, sreg_offsets[index], segments[index]);
    sregs.gdt.base = *vmcb64(vcpu, VMCB_GDTR + 8);
    sregs.gdt.limit = (uint16_t)*vmcb32(vcpu, VMCB_GDTR + 4);
    sregs.idt.base = *vmcb64(vcpu, VMCB_IDTR + 8);
    sregs.idt.limit = (uint16_t)*vmcb32(vcpu, VMCB_IDTR + 4);
    sregs.cr0 = *vmcb64(vcpu, VMCB_CR0);
    sregs.cr2 = *vmcb64(vcpu, VMCB_CR2);
    sregs.cr3 = *vmcb64(vcpu, VMCB_CR3);
    sregs.cr4 = *vmcb64(vcpu, VMCB_CR4);
    sregs.cr8 = vcpu->vm->irqchip ? (apic_get(vcpu, APIC_TASKPRI) >> 4) & 0xFU
                                  : *vmcb64(vcpu, VMCB_INT_CTL) & INT_CTL_V_TPR_MASK;
    sregs.efer = *vmcb64(vcpu, VMCB_EFER) & ~EFER_SVME;
    sregs.apic_base = vcpu->apic_base;
    if (vcpu->irq_pending)
        sregs.interrupt_bitmap[vcpu->irq_vector / 64] |= 1ULL << (vcpu->irq_vector % 64);
    return copy_to_user(argument, &sregs, sizeof(sregs)) == 0 ? 0 : -EFAULT;
}

static int64_t set_sregs(struct kvm_vcpu *vcpu, uint64_t argument) {
    struct kvm_sregs sregs;
    if (copy_from_user(&sregs, argument, sizeof(sregs)) != 0) return -EFAULT;
    if ((sregs.cr0 >> 32) || (sregs.efer & ~(EFER_SCE | EFER_LME | EFER_LMA | EFER_NXE |
                                               EFER_FFXSR)))
        return -EINVAL;
    const struct kvm_segment *segments[8] = {&sregs.cs, &sregs.ds, &sregs.es, &sregs.fs,
                                             &sregs.gs, &sregs.ss, &sregs.tr, &sregs.ldt};
    for (int index = 0; index < 8; index++) segment_set(vcpu, sreg_offsets[index], segments[index]);
    vmcb_segment(vcpu, VMCB_GDTR, 0, 0, sregs.gdt.limit, sregs.gdt.base);
    vmcb_segment(vcpu, VMCB_IDTR, 0, 0, sregs.idt.limit, sregs.idt.base);
    *vmcb64(vcpu, VMCB_CR0) = sregs.cr0;
    *vmcb64(vcpu, VMCB_CR2) = sregs.cr2;
    *vmcb64(vcpu, VMCB_CR3) = sregs.cr3;
    *vmcb64(vcpu, VMCB_CR4) = sregs.cr4;
    *vmcb64(vcpu, VMCB_INT_CTL) =
        (*vmcb64(vcpu, VMCB_INT_CTL) & ~INT_CTL_V_TPR_MASK) | (sregs.cr8 & INT_CTL_V_TPR_MASK);
    *vmcb64(vcpu, VMCB_EFER) = sregs.efer | EFER_SVME;
    vcpu->apic_base = sregs.apic_base;
    if (vcpu->vm->irqchip) apic_set(vcpu, APIC_TASKPRI, (uint32_t)(sregs.cr8 & 0xFU) << 4);
    vcpu->irq_pending = 0;
    for (unsigned vector = 0; vector < 256; vector++) {
        if (sregs.interrupt_bitmap[vector / 64] & (1ULL << (vector % 64))) {
            vcpu->irq_pending = 1;
            vcpu->irq_vector = (uint8_t)vector;
            break;
        }
    }
    vcpu->flush = 1;
    return 0;
}

static int64_t get_regs(struct kvm_vcpu *vcpu, uint64_t argument) {
    struct kvm_regs regs = {
        .rax = get_reg(vcpu, R_AX), .rbx = vcpu->regs[R_BX], .rcx = vcpu->regs[R_CX],
        .rdx = vcpu->regs[R_DX], .rsi = vcpu->regs[R_SI], .rdi = vcpu->regs[R_DI],
        .rsp = get_reg(vcpu, R_SP), .rbp = vcpu->regs[R_BP], .r8 = vcpu->regs[R_8],
        .r9 = vcpu->regs[R_9], .r10 = vcpu->regs[R_10], .r11 = vcpu->regs[R_11],
        .r12 = vcpu->regs[R_12], .r13 = vcpu->regs[R_13], .r14 = vcpu->regs[R_14],
        .r15 = vcpu->regs[R_15], .rip = *vmcb64(vcpu, VMCB_RIP),
        .rflags = *vmcb64(vcpu, VMCB_RFLAGS)};
    return copy_to_user(argument, &regs, sizeof(regs)) == 0 ? 0 : -EFAULT;
}

static int64_t set_regs(struct kvm_vcpu *vcpu, uint64_t argument) {
    struct kvm_regs regs;
    if (copy_from_user(&regs, argument, sizeof(regs)) != 0) return -EFAULT;
    put_reg(vcpu, R_AX, regs.rax);
    vcpu->regs[R_BX] = regs.rbx;
    vcpu->regs[R_CX] = regs.rcx;
    vcpu->regs[R_DX] = regs.rdx;
    vcpu->regs[R_SI] = regs.rsi;
    vcpu->regs[R_DI] = regs.rdi;
    put_reg(vcpu, R_SP, regs.rsp);
    vcpu->regs[R_BP] = regs.rbp;
    vcpu->regs[R_8] = regs.r8;
    vcpu->regs[R_9] = regs.r9;
    vcpu->regs[R_10] = regs.r10;
    vcpu->regs[R_11] = regs.r11;
    vcpu->regs[R_12] = regs.r12;
    vcpu->regs[R_13] = regs.r13;
    vcpu->regs[R_14] = regs.r14;
    vcpu->regs[R_15] = regs.r15;
    *vmcb64(vcpu, VMCB_RIP) = regs.rip;
    *vmcb64(vcpu, VMCB_RFLAGS) = regs.rflags | 2ULL;
    return 0;
}

// The FXSAVE layout inside the guest's XSAVE area.
static int64_t get_fpu(struct kvm_vcpu *vcpu, uint64_t argument) {
    const uint8_t *area = vcpu->guest_fpu;
    struct kvm_fpu fpu;
    memset(&fpu, 0, sizeof(fpu));
    memcpy(&fpu.fcw, area + 0, 2);
    memcpy(&fpu.fsw, area + 2, 2);
    fpu.ftwx = area[4];
    memcpy(&fpu.last_opcode, area + 6, 2);
    memcpy(&fpu.last_ip, area + 8, 8);
    memcpy(&fpu.last_dp, area + 16, 8);
    memcpy(&fpu.mxcsr, area + 24, 4);
    for (int index = 0; index < 8; index++) memcpy(fpu.fpr[index], area + 32 + index * 16, 16);
    memcpy(fpu.xmm, area + 160, sizeof(fpu.xmm));
    return copy_to_user(argument, &fpu, sizeof(fpu)) == 0 ? 0 : -EFAULT;
}

static int64_t set_fpu(struct kvm_vcpu *vcpu, uint64_t argument) {
    struct kvm_fpu fpu;
    if (copy_from_user(&fpu, argument, sizeof(fpu)) != 0) return -EFAULT;
    if (fpu.mxcsr & ~guest_mxcsr_mask) return -EINVAL;
    uint8_t *area = vcpu->guest_fpu;
    memcpy(area + 0, &fpu.fcw, 2);
    memcpy(area + 2, &fpu.fsw, 2);
    area[4] = fpu.ftwx;
    memcpy(area + 6, &fpu.last_opcode, 2);
    memcpy(area + 8, &fpu.last_ip, 8);
    memcpy(area + 16, &fpu.last_dp, 8);
    memcpy(area + 24, &fpu.mxcsr, 4);
    for (int index = 0; index < 8; index++) memcpy(area + 32 + index * 16, fpu.fpr[index], 16);
    memcpy(area + 160, fpu.xmm, sizeof(fpu.xmm));
    // x87 and SSE come from the legacy area now, whatever the header said.
    uint64_t features;
    memcpy(&features, area + 512, 8);
    features |= 3ULL;
    memcpy(area + 512, &features, 8);
    return 0;
}

static int64_t get_xsave(struct kvm_vcpu *vcpu, uint64_t argument) {
    struct kvm_xsave *xsave = kmalloc(sizeof(*xsave));
    if (!xsave) return -ENOMEM;
    memset(xsave, 0, sizeof(*xsave));
    memcpy(xsave->region, vcpu->guest_fpu, xsave_size_for(host_xcr0));
    int64_t status = copy_to_user(argument, xsave, sizeof(*xsave)) == 0 ? 0 : -EFAULT;
    kfree(xsave);
    return status;
}

// A state from userspace, loaded by XRSTOR on the next entry: what would make XRSTOR
// fault is refused.
static int64_t set_xsave(struct kvm_vcpu *vcpu, uint64_t argument) {
    struct kvm_xsave *xsave = kmalloc(sizeof(*xsave));
    if (!xsave) return -ENOMEM;
    int64_t status = 0;
    const uint8_t *area = (const uint8_t *)xsave->region;
    uint64_t features, compacted;
    uint32_t mxcsr;
    if (copy_from_user(xsave, argument, sizeof(*xsave)) != 0) {
        status = -EFAULT;
        goto out;
    }
    memcpy(&mxcsr, area + 24, 4);
    memcpy(&features, area + 512, 8);
    memcpy(&compacted, area + 520, 8);
    if ((mxcsr & ~guest_mxcsr_mask) || (features & ~host_xcr0) || compacted) {
        status = -EINVAL;
        goto out;
    }
    for (unsigned at = 528; at < 576; at++) {
        if (area[at]) {
            status = -EINVAL;
            goto out;
        }
    }
    memcpy(vcpu->guest_fpu, area, xsave_size_for(host_xcr0));
out:
    kfree(xsave);
    return status;
}

static int64_t get_events(struct kvm_vcpu *vcpu, uint64_t argument) {
    struct kvm_vcpu_events events;
    memset(&events, 0, sizeof(events));
    uint64_t pending = vcpu->reinject;
    uint64_t type = (pending >> EVENT_TYPE_SHIFT) & 7ULL;
    if ((pending & EVENT_VALID) && type == EVENT_TYPE_EXCEPTION) {
        events.exception.injected = 1;
        events.exception.nr = (uint8_t)pending;
        events.exception.has_error_code = (pending & EVENT_ERROR_VALID) != 0;
        events.exception.error_code = (uint32_t)(pending >> 32);
    } else if (vcpu->exception_pending) {
        events.exception.injected = 1;
        events.exception.nr = vcpu->exception_vector;
        events.exception.has_error_code = (uint8_t)vcpu->exception_has_error;
        events.exception.error_code = vcpu->exception_error;
    }
    if ((pending & EVENT_VALID) && type == EVENT_TYPE_INTR) {
        events.interrupt.injected = 1;
        events.interrupt.nr = (uint8_t)pending;
    } else if (vcpu->irq_pending) {
        events.interrupt.injected = 1;
        events.interrupt.nr = vcpu->irq_vector;
    }
    events.interrupt.shadow = (uint8_t)(*vmcb64(vcpu, VMCB_INT_STATE) & 1ULL);
    events.nmi.injected = (pending & EVENT_VALID) && type == EVENT_TYPE_NMI;
    events.nmi.pending = (uint8_t)vcpu->nmi_pending;
    events.nmi.masked = (uint8_t)vcpu->nmi_masked;
    events.sipi_vector = vcpu->sipi_vector;
    events.flags = KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SIPI_VECTOR |
        KVM_VCPUEVENT_VALID_SHADOW;
    return copy_to_user(argument, &events, sizeof(events)) == 0 ? 0 : -EFAULT;
}

static int64_t set_events(struct kvm_vcpu *vcpu, uint64_t argument) {
    struct kvm_vcpu_events events;
    if (copy_from_user(&events, argument, sizeof(events)) != 0) return -EFAULT;
    vcpu->reinject = 0;
    vcpu->exception_pending = events.exception.injected || events.exception.pending;
    vcpu->exception_vector = events.exception.nr;
    vcpu->exception_has_error = events.exception.has_error_code;
    vcpu->exception_error = events.exception.error_code;
    vcpu->irq_pending = events.interrupt.injected;
    vcpu->irq_vector = events.interrupt.nr;
    if (events.flags & KVM_VCPUEVENT_VALID_SHADOW) {
        uint64_t *state = vmcb64(vcpu, VMCB_INT_STATE);
        *state = (*state & ~1ULL) | (events.interrupt.shadow & 1U);
    }
    vcpu->nmi_pending = events.nmi.injected;
    if (events.flags & KVM_VCPUEVENT_VALID_NMI_PENDING)
        vcpu->nmi_pending |= events.nmi.pending;
    vcpu->nmi_masked = events.nmi.masked;
    vcpu->awaiting_iret = 0;
    if (events.flags & KVM_VCPUEVENT_VALID_SIPI_VECTOR) vcpu->sipi_vector = events.sipi_vector;
    return 0;
}

static int64_t vcpu_msrs(struct kvm_vcpu *vcpu, uint64_t argument, int write) {
    struct kvm_msrs header;
    if (copy_from_user(&header, argument, sizeof(header)) != 0) return -EFAULT;
    if (header.nmsrs > 4096) return -E2BIG;
    for (uint32_t index = 0; index < header.nmsrs; index++) {
        struct kvm_msr_entry entry;
        uint64_t at = argument + sizeof(header) + (uint64_t)index * sizeof(entry);
        if (copy_from_user(&entry, at, sizeof(entry)) != 0) return -EFAULT;
        if (write) {
            if (msr_write(vcpu, entry.index, entry.data, 1) != 0) return index;
        } else {
            if (msr_read(vcpu, entry.index, &entry.data, 1) != 0) return index;
            if (copy_to_user(at, &entry, sizeof(entry)) != 0) return -EFAULT;
        }
    }
    return header.nmsrs;
}

static int64_t set_cpuid(struct kvm_vcpu *vcpu, uint64_t argument) {
    struct kvm_cpuid2 header;
    if (copy_from_user(&header, argument, sizeof(header)) != 0) return -EFAULT;
    if (header.nent > KVM_CPUID_MAX) return -E2BIG;
    size_t bytes = (size_t)header.nent * sizeof(struct kvm_cpuid_entry2);
    struct kvm_cpuid_entry2 *entries = header.nent ? kmalloc(bytes) : NULL;
    if (header.nent && !entries) return -ENOMEM;
    if (bytes && copy_from_user(entries, argument + sizeof(header), bytes) != 0) {
        kfree(entries);
        return -EFAULT;
    }
    kfree(vcpu->cpuid);
    vcpu->cpuid = entries;
    vcpu->cpuid_count = header.nent;
    return 0;
}

static int64_t get_cpuid(struct kvm_vcpu *vcpu, uint64_t argument) {
    struct kvm_cpuid2 header;
    if (copy_from_user(&header, argument, sizeof(header)) != 0) return -EFAULT;
    uint32_t wanted = header.nent;
    header.nent = vcpu->cpuid_count;
    if (copy_to_user(argument, &header, sizeof(header)) != 0) return -EFAULT;
    if (wanted < vcpu->cpuid_count) return -E2BIG;
    size_t bytes = (size_t)vcpu->cpuid_count * sizeof(struct kvm_cpuid_entry2);
    if (bytes && copy_to_user(argument + sizeof(header), vcpu->cpuid, bytes) != 0) return -EFAULT;
    return 0;
}

static int64_t vcpu_ioctl(struct file *file, unsigned long request, uint64_t argument) {
    struct kvm_vcpu *vcpu = file->private_data;
    if (!vcpu) return -EBADF;
    int64_t status;
    mutex_lock(&vcpu->lock);
    switch (request) {
    case KVM_RUN:          status = argument ? -EINVAL : vcpu_run(vcpu); break;
    case KVM_GET_REGS:     status = get_regs(vcpu, argument); break;
    case KVM_SET_REGS:     status = set_regs(vcpu, argument); break;
    case KVM_GET_SREGS:    status = get_sregs(vcpu, argument); break;
    case KVM_SET_SREGS:    status = set_sregs(vcpu, argument); break;
    case KVM_GET_FPU:      status = get_fpu(vcpu, argument); break;
    case KVM_SET_FPU:      status = set_fpu(vcpu, argument); break;
    case KVM_GET_XSAVE:    status = get_xsave(vcpu, argument); break;
    case KVM_SET_XSAVE:    status = set_xsave(vcpu, argument); break;
    case KVM_GET_MSRS:     status = vcpu_msrs(vcpu, argument, 0); break;
    case KVM_SET_MSRS:     status = vcpu_msrs(vcpu, argument, 1); break;
    case KVM_SET_CPUID2:   status = set_cpuid(vcpu, argument); break;
    case KVM_GET_CPUID2:   status = get_cpuid(vcpu, argument); break;
    case KVM_GET_VCPU_EVENTS: status = get_events(vcpu, argument); break;
    case KVM_SET_VCPU_EVENTS: status = set_events(vcpu, argument); break;
    case KVM_INTERRUPT: {
        struct kvm_interrupt interrupt;
        if (copy_from_user(&interrupt, argument, sizeof(interrupt)) != 0) status = -EFAULT;
        else if (interrupt.irq >= 256) status = -EINVAL;
        else if (vcpu->vm->irqchip == IRQCHIP_FULL) status = -ENXIO;
        else if (vcpu->irq_pending) status = -EEXIST;
        else {
            vcpu->irq_pending = 1;
            vcpu->irq_vector = (uint8_t)interrupt.irq;
            status = 0;
        }
        break;
    }
    case KVM_NMI:
        vcpu->nmi_pending = 1;
        status = 0;
        break;
    case KVM_GET_LAPIC: {
        if (!vcpu->vm->irqchip) {
            status = -ENXIO;
            break;
        }
        struct kvm_lapic_state *state = kmalloc(sizeof(*state));
        if (!state) {
            status = -ENOMEM;
            break;
        }
        memcpy(state->regs, vcpu->apic, sizeof(state->regs));
        uint32_t current = apic_timer_current(vcpu);
        memcpy(state->regs + APIC_TMCCT, &current, 4);
        status = copy_to_user(argument, state, sizeof(*state)) == 0 ? 0 : -EFAULT;
        kfree(state);
        break;
    }
    case KVM_SET_LAPIC: {
        if (!vcpu->vm->irqchip) {
            status = -ENXIO;
            break;
        }
        struct kvm_lapic_state *state = kmalloc(sizeof(*state));
        if (!state) {
            status = -ENOMEM;
            break;
        }
        if (copy_from_user(state, argument, sizeof(*state)) != 0) {
            status = -EFAULT;
        } else {
            for (unsigned offset = 0; offset < sizeof(state->regs); offset += 4) {
                uint32_t word;
                memcpy(&word, state->regs + offset, 4);
                apic_set(vcpu, offset, word);
            }
            apic_set(vcpu, APIC_LVR, APIC_VERSION);
            apic_timer_start(vcpu);
            vcpu_kick(vcpu);
            status = 0;
        }
        kfree(state);
        break;
    }
    case KVM_GET_XCRS: {
        struct kvm_xcrs xcrs;
        memset(&xcrs, 0, sizeof(xcrs));
        xcrs.nr_xcrs = 1;
        xcrs.xcrs[0].value = vcpu->xcr0;
        status = copy_to_user(argument, &xcrs, sizeof(xcrs)) == 0 ? 0 : -EFAULT;
        break;
    }
    case KVM_SET_XCRS: {
        struct kvm_xcrs xcrs;
        status = 0;
        if (copy_from_user(&xcrs, argument, sizeof(xcrs)) != 0) status = -EFAULT;
        else if (xcrs.nr_xcrs > 16) status = -EINVAL;
        for (uint32_t index = 0; !status && index < xcrs.nr_xcrs; index++) {
            if (xcrs.xcrs[index].xcr != 0 || !xcr0_valid(xcrs.xcrs[index].value)) status = -EINVAL;
            else vcpu->xcr0 = xcrs.xcrs[index].value;
        }
        break;
    }
    case KVM_GET_MP_STATE: {
        struct kvm_mp_state state = {vcpu->mp_state};
        status = copy_to_user(argument, &state, sizeof(state)) == 0 ? 0 : -EFAULT;
        break;
    }
    case KVM_SET_MP_STATE: {
        struct kvm_mp_state state;
        if (copy_from_user(&state, argument, sizeof(state)) != 0) status = -EFAULT;
        else if (state.mp_state > 8) status = -EINVAL;
        else {
            vcpu->mp_state = state.mp_state;
            status = 0;
        }
        break;
    }
    case KVM_GET_DEBUGREGS: {
        struct kvm_debugregs regs;
        memset(&regs, 0, sizeof(regs));
        memcpy(regs.db, vcpu->dr, sizeof(regs.db));
        regs.dr6 = *vmcb64(vcpu, VMCB_DR6);
        regs.dr7 = *vmcb64(vcpu, VMCB_DR7);
        status = copy_to_user(argument, &regs, sizeof(regs)) == 0 ? 0 : -EFAULT;
        break;
    }
    case KVM_SET_DEBUGREGS: {
        struct kvm_debugregs regs;
        if (copy_from_user(&regs, argument, sizeof(regs)) != 0) status = -EFAULT;
        else if (regs.flags || (regs.dr6 >> 32) || (regs.dr7 >> 32)) status = -EINVAL;
        else {
            memcpy(vcpu->dr, regs.db, sizeof(vcpu->dr));
            *vmcb64(vcpu, VMCB_DR6) = regs.dr6;
            *vmcb64(vcpu, VMCB_DR7) = regs.dr7;
            // Loaded again on the next entry wherever it runs.
            vcpu->serial = __atomic_add_fetch(&vcpu_serials, 1, __ATOMIC_RELAXED);
            status = 0;
        }
        break;
    }
    case KVM_X86_SETUP_MCE: {
        uint64_t cap;
        uint64_t banks;
        if (copy_from_user(&cap, argument, sizeof(cap)) != 0) {
            status = -EFAULT;
            break;
        }
        banks = cap & 0xFFULL;
        if (!banks || banks > KVM_MCE_BANKS || (cap & ~(MCG_CTL_P | MCG_SER_P | 0xFFULL))) {
            status = -EINVAL;
            break;
        }
        vcpu->mcg_cap = cap;
        vcpu->mcg_ctl = (cap & MCG_CTL_P) ? ~0ULL : 0;
        for (uint64_t bank = 0; bank < banks; bank++) vcpu->mce_banks[bank * 4] = ~0ULL;
        status = 0;
        break;
    }
    case KVM_GET_TSC_KHZ: status = (int64_t)(time_tsc_frequency() / 1000ULL); break;
    case KVM_SET_TSC_KHZ: {
        // Without TSC scaling the guest runs at the host's rate; other rates are refused.
        uint64_t host_khz = time_tsc_frequency() / 1000ULL;
        uint64_t difference = argument > host_khz ? argument - host_khz : host_khz - argument;
        status = difference * 4000ULL <= host_khz ? 0 : -EINVAL;
        break;
    }
    case KVM_SET_GUEST_DEBUG: {
        uint32_t control;
        if (copy_from_user(&control, argument, sizeof(control)) != 0) status = -EFAULT;
        else status = (control & KVM_GUESTDBG_ENABLE) ? -EINVAL : 0;
        break;
    }
    case KVM_ENABLE_CAP:
    case KVM_SET_VAPIC_ADDR: {
        // The paravirtual TPR page is not offered (no KVM_CAP_VAPIC): only clearing it is.
        uint64_t address;
        if (copy_from_user(&address, argument, sizeof(address)) != 0) status = -EFAULT;
        else status = address ? -EINVAL : 0;
        break;
    }
    case KVM_GET_STATS_FD:
        status = -ENOTTY;
        break;
    case KVM_TPR_ACCESS_REPORTING:
    case KVM_KVMCLOCK_CTRL:
    case KVM_X86_SET_MCE:  status = -EINVAL; break;
    default:
        kprintf("kvm: vCPU ioctl 0x%x is not supported\n", (unsigned)request);
        status = -ENOTTY;
        break;
    }
    mutex_unlock(&vcpu->lock);
    return status;
}

static int64_t vcpu_mmap(struct vfs_node *node, struct file *file, uint64_t cr3,
                         uint64_t virtual_address, uint64_t length, uint64_t offset,
                         uint64_t page_flags) {
    (void)node;
    struct kvm_vcpu *vcpu = file->private_data;
    if (!vcpu || offset || !length || length > KVM_RUN_PAGES * 4096ULL) return -EINVAL;
    uint64_t flags = page_flags | PAGE_USER | PAGE_PRESENT | PAGE_NX;
    for (uint64_t mapped = 0; mapped < length; mapped += 4096ULL) {
        uint64_t physical = vcpu->run_pages[mapped / 4096ULL];
        if (pmm_page_ref(physical) != 0 ||
            vmm_map_page_in(cr3, virtual_address + mapped, physical, flags) != 0) {
            while (mapped) {
                mapped -= 4096ULL;
                (void)vmm_unmap_page_in(cr3, virtual_address + mapped);
                pmm_free_page((void *)(uintptr_t)vcpu->run_pages[mapped / 4096ULL]);
            }
            return -EINVAL;
        }
    }
    return 0;
}

// ---- VMs ----

static void vm_put(struct kvm_vm *vm) {
    if (__atomic_sub_fetch(&vm->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    npt_free_table(vm->npt_root, 3);
    while (vm->irqfds) {
        struct irqfd *irqfd = vm->irqfds;
        vm->irqfds = irqfd->next;
        eventfd_set_consumer(irqfd->file->eventfd, NULL, NULL);
        file_unref(irqfd->file);
        kfree(irqfd);
    }
    kfree(vm->routes);
    while (vm->ioeventfds) {
        struct ioeventfd *item = vm->ioeventfds;
        vm->ioeventfds = item->next;
        file_unref(item->file);
        kfree(item);
    }
    kfree(vm);
}

static void vcpu_free(struct kvm_vcpu *vcpu) {
    if (vcpu->vmcb_pa) pmm_free_page((void *)(uintptr_t)vcpu->vmcb_pa);
    for (unsigned page = 0; page < KVM_RUN_PAGES; page++)
        if (vcpu->run_pages[page]) pmm_free_page((void *)(uintptr_t)vcpu->run_pages[page]);
    kfree(vcpu->fpu_allocation);
    kfree(vcpu->cpuid);
    kfree(vcpu->apic);
    kfree(vcpu);
}

static void vcpu_release(struct file *file) {
    struct kvm_vcpu *vcpu = file->private_data;
    if (!vcpu) return;
    file->private_data = NULL;
    struct kvm_vm *vm = vcpu->vm;
    mutex_lock(&vm->lock);
    mutex_lock(&vm->irq_lock);
    if (vcpu->id < KVM_MAX_VCPUS && vm->vcpus[vcpu->id] == vcpu) vm->vcpus[vcpu->id] = NULL;
    mutex_unlock(&vm->irq_lock);
    mutex_unlock(&vm->lock);
    vcpu_free(vcpu);
    vm_put(vm);
}

static const struct vfs_file_ops vcpu_file_ops = {
    .release = vcpu_release,
    .ioctl = vcpu_ioctl,
};

static int install_fd(struct vfs_node *node, void *object) {
    struct file *file = file_open_node(node, 2 /* O_RDWR */);
    if (!file) return -ENOMEM;
    file->private_data = object;
    int fd = process_install_file_flags(process_current(), file, 0, PROCESS_FD_CLOEXEC);
    if (fd < 0) {
        file->private_data = NULL;
        file_unref(file);
        return -EMFILE;
    }
    return fd;
}

static int64_t create_vcpu(struct kvm_vm *vm, uint64_t id) {
    if (id >= KVM_MAX_VCPUS) return -EINVAL;
    struct kvm_vcpu *vcpu = kmalloc(sizeof(*vcpu));
    if (!vcpu) return -ENOMEM;
    memset(vcpu, 0, sizeof(*vcpu));
    mutex_init(&vcpu->lock, "kvm vcpu", LOCK_RANK_KVM_VCPU);
    vcpu->vm = vm;
    vcpu->id = (uint32_t)id;
    vcpu->serial = __atomic_add_fetch(&vcpu_serials, 1, __ATOMIC_RELAXED);
    vcpu->vmcb_pa = alloc_zeroed_page();
    for (unsigned page = 0; page < KVM_RUN_PAGES; page++) vcpu->run_pages[page] = alloc_zeroed_page();
    vcpu->fpu_allocation = kmalloc(2 * (PROCESS_FPU_STATE_SIZE + 64));
    vcpu->apic = kmalloc(1024);
    if (!vcpu->vmcb_pa || !vcpu->run_pages[0] || !vcpu->run_pages[1] || !vcpu->fpu_allocation ||
        !vcpu->apic) {
        vcpu_free(vcpu);
        return -ENOMEM;
    }
    vcpu->vmcb = vmm_phys_to_virt(vcpu->vmcb_pa);
    vcpu->run = vmm_phys_to_virt(vcpu->run_pages[0]);
    vcpu->io_data = vmm_phys_to_virt(vcpu->run_pages[1]);
    uintptr_t aligned = ((uintptr_t)vcpu->fpu_allocation + 63U) & ~(uintptr_t)63U;
    vcpu->guest_fpu = (uint8_t *)aligned;
    vcpu->host_fpu = vcpu->guest_fpu + PROCESS_FPU_STATE_SIZE;
    arch_fpu_init(vcpu->guest_fpu);
    vcpu->xcr0 = 1;
    vcpu->apic_base = 0xFEE00000ULL | (1ULL << 11) | (id == 0 ? 1ULL << 8 : 0);
    vcpu->misc_enable = 1;
    memset(vcpu->apic, 0, 1024);
    apic_set(vcpu, APIC_ID, (uint32_t)id << 24);
    apic_reset(vcpu);
    vmcb_init_control(vcpu);
    vmcb_init_state(vcpu);

    mutex_lock(&vm->lock);
    // With the APIC here, application processors wait for INIT and SIPI from the guest.
    vcpu->mp_state = vm->irqchip && id != 0 ? KVM_MP_STATE_UNINITIALIZED : KVM_MP_STATE_RUNNABLE;
    if (vm->vcpus[id]) {
        mutex_unlock(&vm->lock);
        vcpu_free(vcpu);
        return -EEXIST;
    }
    mutex_lock(&vm->irq_lock);
    vm->vcpus[id] = vcpu;
    mutex_unlock(&vm->irq_lock);
    vm->vcpus_created++;
    __atomic_add_fetch(&vm->refs, 1, __ATOMIC_ACQ_REL);
    mutex_unlock(&vm->lock);

    int fd = install_fd(vcpu_node, vcpu);
    if (fd < 0) {
        mutex_lock(&vm->lock);
        mutex_lock(&vm->irq_lock);
        vm->vcpus[id] = NULL;
        mutex_unlock(&vm->irq_lock);
        mutex_unlock(&vm->lock);
        vcpu_free(vcpu);
        vm_put(vm);
    }
    return fd;
}

static void slot_remove(struct kvm_vm *vm, struct memory_slot *slot) {
    npt_unmap(vm, slot->gpa, slot->size);
    uint32_t index = (uint32_t)(slot - vm->slots);
    vm->slots[index] = vm->slots[--vm->slot_count];
}

static int64_t set_memory_region(struct kvm_vm *vm, uint64_t argument) {
    struct kvm_userspace_memory_region region;
    if (copy_from_user(&region, argument, sizeof(region)) != 0) return -EFAULT;
    if (region.slot >> 16) return -EINVAL;
    if (region.flags & ~(uint32_t)KVM_MEM_LOG_DIRTY_PAGES) return -EINVAL;
    if ((region.guest_phys_addr | region.memory_size | region.userspace_addr) & 0xFFFULL)
        return -EINVAL;
    if (region.memory_size &&
        (region.guest_phys_addr >= GPA_LIMIT ||
         region.memory_size > GPA_LIMIT - region.guest_phys_addr ||
         region.userspace_addr >= USER_ADDRESS_LIMIT ||
         region.memory_size > USER_ADDRESS_LIMIT - region.userspace_addr))
        return -EINVAL;
    mutex_lock(&vm->lock);
    int64_t status = 0;
    struct memory_slot *existing = NULL;
    for (uint32_t index = 0; index < vm->slot_count; index++)
        if (vm->slots[index].id == region.slot) existing = &vm->slots[index];
    if (existing && existing->gpa == region.guest_phys_addr &&
        existing->size == region.memory_size && existing->hva == region.userspace_addr) {
        existing->flags = region.flags;
        goto out;
    }
    if (existing) slot_remove(vm, existing);
    if (!region.memory_size) goto out;
    for (uint32_t index = 0; index < vm->slot_count; index++) {
        const struct memory_slot *other = &vm->slots[index];
        if (region.guest_phys_addr < other->gpa + other->size &&
            other->gpa < region.guest_phys_addr + region.memory_size) {
            status = -EEXIST;
            goto out;
        }
    }
    if (vm->slot_count >= KVM_SLOTS) {
        status = -ENOSPC;
        goto out;
    }
    vm->slots[vm->slot_count++] = (struct memory_slot){
        .id = region.slot,
        .flags = region.flags,
        .gpa = region.guest_phys_addr,
        .size = region.memory_size,
        .hva = region.userspace_addr,
    };
out:
    mutex_unlock(&vm->lock);
    return status;
}

// Every page is reported dirty: memory the guest writes through the nested page tables
// leaves no trace this tracks, so all of it may have changed.
static int64_t get_dirty_log(struct kvm_vm *vm, uint64_t argument) {
    struct kvm_dirty_log log;
    if (copy_from_user(&log, argument, sizeof(log)) != 0) return -EFAULT;
    uint64_t pages = 0;
    int found = 0;
    mutex_lock(&vm->lock);
    for (uint32_t index = 0; index < vm->slot_count; index++) {
        if (vm->slots[index].id == log.slot) {
            pages = vm->slots[index].size / 4096ULL;
            found = 1;
        }
    }
    mutex_unlock(&vm->lock);
    if (!found) return -ENOENT;
    for (uint64_t word = 0; word < (pages + 63ULL) / 64ULL; word++) {
        uint64_t bits = pages - word * 64ULL >= 64ULL ? ~0ULL : (1ULL << (pages % 64ULL)) - 1ULL;
        if (copy_to_user(log.dirty_bitmap + word * 8ULL, &bits, 8) != 0) return -EFAULT;
    }
    return 0;
}

static int64_t set_ioeventfd(struct kvm_vm *vm, uint64_t argument) {
    struct kvm_ioeventfd request;
    if (copy_from_user(&request, argument, sizeof(request)) != 0) return -EFAULT;
    uint32_t known = KVM_IOEVENTFD_FLAG_DATAMATCH | KVM_IOEVENTFD_FLAG_PIO |
        KVM_IOEVENTFD_FLAG_DEASSIGN;
    if (request.flags & ~known) return -EINVAL;
    if (request.len != 0 && request.len != 1 && request.len != 2 && request.len != 4 &&
        request.len != 8)
        return -EINVAL;
    int pio = (request.flags & KVM_IOEVENTFD_FLAG_PIO) != 0;
    int match = (request.flags & KVM_IOEVENTFD_FLAG_DATAMATCH) != 0;
    if (pio && request.addr > 0xFFFFULL) return -EINVAL;
    struct file *file = process_file_get(process_current(), request.fd);
    if (!file) return -EBADF;
    if (file->kind != FILE_KIND_EVENTFD) {
        file_unref(file);
        return -EINVAL;
    }
    int64_t status = 0;
    mutex_lock(&vm->lock);
    struct ioeventfd **link = &vm->ioeventfds;
    for (; *link; link = &(*link)->next) {
        struct ioeventfd *item = *link;
        if (item->pio == pio && item->address == request.addr && item->length == request.len &&
            item->match == match && (!match || item->datamatch == request.datamatch))
            break;
    }
    if (request.flags & KVM_IOEVENTFD_FLAG_DEASSIGN) {
        if (!*link || (*link)->file != file) {
            status = -ENOENT;
        } else {
            struct ioeventfd *item = *link;
            *link = item->next;
            file_unref(item->file);
            kfree(item);
        }
        file_unref(file);
    } else if (*link) {
        status = -EEXIST;
        file_unref(file);
    } else {
        struct ioeventfd *item = kmalloc(sizeof(*item));
        if (!item) {
            status = -ENOMEM;
            file_unref(file);
        } else {
            *item = (struct ioeventfd){
                .next = vm->ioeventfds,
                .address = request.addr,
                .datamatch = request.datamatch,
                .length = request.len,
                .pio = pio,
                .match = match,
                .file = file,
            };
            vm->ioeventfds = item;
        }
    }
    mutex_unlock(&vm->lock);
    return status;
}

static int64_t check_extension(uint64_t capability) {
    switch (capability) {
    case KVM_CAP_USER_MEMORY: case KVM_CAP_SET_TSS_ADDR: case KVM_CAP_EXT_CPUID:
    case KVM_CAP_MP_STATE: case KVM_CAP_DESTROY_MEMORY_REGION_WORKS:
    case KVM_CAP_JOIN_MEMORY_REGIONS_WORKS: case KVM_CAP_IOEVENTFD:
    case KVM_CAP_INTERNAL_ERROR_DATA: case KVM_CAP_VCPU_EVENTS:
    case KVM_CAP_SET_IDENTITY_MAP_ADDR: case KVM_CAP_XSAVE: case KVM_CAP_DEBUGREGS:
    case KVM_CAP_X86_ROBUST_SINGLESTEP: case KVM_CAP_XCRS: case KVM_CAP_GET_TSC_KHZ:
    case KVM_CAP_IOEVENTFD_ANY_LENGTH: case KVM_CAP_IMMEDIATE_EXIT: case KVM_CAP_EXT_EMUL_CPUID:
    case KVM_CAP_ADJUST_CLOCK:
        return 1;
    // The interrupt controller can be in the kernel, all of it or only the local APICs.
    case KVM_CAP_IRQCHIP: case KVM_CAP_SPLIT_IRQCHIP: case KVM_CAP_IRQFD:
    case KVM_CAP_SIGNAL_MSI: case KVM_CAP_IRQ_INJECT_STATUS:
    case KVM_CAP_PIT2: case KVM_CAP_PIT_STATE2: case KVM_CAP_REINJECT_CONTROL:
        return 1;
    case KVM_CAP_IRQ_ROUTING:  return KVM_MAX_ROUTES;
    case KVM_CAP_MCE:          return KVM_MCE_BANKS;
    case KVM_CAP_NR_MEMSLOTS:  return KVM_SLOTS;
    case KVM_CAP_NR_VCPUS: {
        unsigned count = smp_cpu_count();
        return count > KVM_MAX_VCPUS ? KVM_MAX_VCPUS : count;
    }
    case KVM_CAP_MAX_VCPUS: case KVM_CAP_MAX_VCPU_ID: return KVM_MAX_VCPUS;
    default:                   return 0;
    }
}

// KVM_CAP_SPLIT_IRQCHIP puts the local APICs in the kernel, before any vCPU exists;
// args[0] is how many GSIs the userspace I/O APIC keeps for its pins.
static int64_t vm_enable_cap(struct kvm_vm *vm, uint64_t argument) {
    struct kvm_enable_cap request;
    if (copy_from_user(&request, argument, sizeof(request)) != 0) return -EFAULT;
    if (request.flags || request.cap != KVM_CAP_SPLIT_IRQCHIP) return -EINVAL;
    if (request.args[0] > KVM_IOAPIC_NUM_PINS) return -EINVAL;
    int64_t status = 0;
    mutex_lock(&vm->lock);
    if (vm->vcpus_created || vm->irqchip) {
        status = -EINVAL;
    } else {
        vm->irqchip = IRQCHIP_SPLIT;
        vm->ioapic_pins = (uint32_t)request.args[0];
    }
    mutex_unlock(&vm->lock);
    return status;
}

static int64_t create_irqchip(struct kvm_vm *vm) {
    int64_t status = 0;
    mutex_lock(&vm->lock);
    if (vm->irqchip) {
        status = -EEXIST;
    } else if (vm->vcpus_created) {
        status = -EINVAL;
    } else {
        mutex_lock(&vm->irq_lock);
        memset(vm->pic, 0, sizeof(vm->pic));
        vm->pic[0].elcr_mask = 0xF8;
        vm->pic[1].elcr_mask = 0xDE;
        memset(&vm->ioapic, 0, sizeof(vm->ioapic));
        for (unsigned pin = 0; pin < KVM_IOAPIC_NUM_PINS; pin++)
            vm->ioapic.redirection[pin] = IOAPIC_MASKED;
        status = default_routes(vm);
        if (!status) vm->irqchip = IRQCHIP_FULL;
        mutex_unlock(&vm->irq_lock);
    }
    mutex_unlock(&vm->lock);
    return status;
}

static int64_t irqchip_state(struct kvm_vm *vm, uint64_t argument, int set) {
    struct kvm_irqchip chip;
    if (copy_from_user(&chip, argument, sizeof(chip)) != 0) return -EFAULT;
    if (vm->irqchip != IRQCHIP_FULL) return -ENXIO;
    if (chip.chip_id > KVM_IRQCHIP_IOAPIC) return -EINVAL;
    mutex_lock(&vm->irq_lock);
    if (chip.chip_id <= KVM_IRQCHIP_PIC_SLAVE) {
        if (set) vm->pic[chip.chip_id] = chip.chip.pic;
        else chip.chip.pic = vm->pic[chip.chip_id];
        if (set) pic_notify(vm);
    } else if (set) {
        vm->ioapic.ioregsel = chip.chip.ioapic.ioregsel;
        vm->ioapic.id = chip.chip.ioapic.id;
        vm->ioapic.irr = chip.chip.ioapic.irr;
        memcpy(vm->ioapic.redirection, chip.chip.ioapic.redirtbl, sizeof(vm->ioapic.redirection));
    } else {
        chip.chip.ioapic.base_address = IOAPIC_BASE;
        chip.chip.ioapic.ioregsel = vm->ioapic.ioregsel;
        chip.chip.ioapic.id = vm->ioapic.id;
        chip.chip.ioapic.irr = vm->ioapic.irr;
        memcpy(chip.chip.ioapic.redirtbl, vm->ioapic.redirection, sizeof(vm->ioapic.redirection));
    }
    mutex_unlock(&vm->irq_lock);
    if (set) return 0;
    return copy_to_user(argument, &chip, sizeof(chip)) == 0 ? 0 : -EFAULT;
}

static int64_t create_pit(struct kvm_vm *vm, uint64_t argument) {
    struct kvm_pit_config config;
    if (copy_from_user(&config, argument, sizeof(config)) != 0) return -EFAULT;
    if (vm->irqchip != IRQCHIP_FULL) return -ENOENT;
    mutex_lock(&vm->irq_lock);
    int64_t status = vm->pit_created ? -EEXIST : 0;
    if (!status) {
        pit_reset(vm);
        vm->pit.flags = config.flags;
        vm->pit_created = 1;
    }
    mutex_unlock(&vm->irq_lock);
    return status;
}

static int64_t pit_state(struct kvm_vm *vm, uint64_t argument, int set) {
    struct kvm_pit_state2 state;
    if (copy_from_user(&state, argument, sizeof(state)) != 0) return -EFAULT;
    if (!vm->pit_created) return -ENXIO;
    mutex_lock(&vm->irq_lock);
    if (set) {
        memcpy(vm->pit.channels, state.channels, sizeof(vm->pit.channels));
        vm->pit.flags = state.flags;
        // As on Linux, the counts start again from now.
        for (unsigned index = 0; index < 3; index++)
            pit_load(vm, index, vm->pit.channels[index].count);
    } else {
        memcpy(state.channels, vm->pit.channels, sizeof(state.channels));
        state.flags = vm->pit.flags;
    }
    mutex_unlock(&vm->irq_lock);
    if (set) return 0;
    return copy_to_user(argument, &state, sizeof(state)) == 0 ? 0 : -EFAULT;
}

static int64_t vm_ioctl(struct file *file, unsigned long request, uint64_t argument) {
    struct kvm_vm *vm = file->private_data;
    if (!vm) return -EBADF;
    switch (request) {
    case KVM_CREATE_VCPU:            return create_vcpu(vm, argument);
    case KVM_SET_USER_MEMORY_REGION: return set_memory_region(vm, argument);
    case KVM_GET_DIRTY_LOG:          return get_dirty_log(vm, argument);
    case KVM_IOEVENTFD:              return set_ioeventfd(vm, argument);
    case KVM_CHECK_EXTENSION:        return check_extension(argument);
    case KVM_SET_TSS_ADDR:           return 0;
    case KVM_SET_NR_MMU_PAGES:       return 0;
    case KVM_SET_IDENTITY_MAP_ADDR: {
        uint64_t address;
        return copy_from_user(&address, argument, sizeof(address)) == 0 ? 0 : -EFAULT;
    }
    case KVM_GET_CLOCK: {
        struct kvm_clock_data clock;
        memset(&clock, 0, sizeof(clock));
        clock.clock = time_uptime_ns() + (uint64_t)vm->clock_offset;
        clock.flags = KVM_CLOCK_TSC_STABLE;
        return copy_to_user(argument, &clock, sizeof(clock)) == 0 ? 0 : -EFAULT;
    }
    case KVM_SET_CLOCK: {
        struct kvm_clock_data clock;
        if (copy_from_user(&clock, argument, sizeof(clock)) != 0) return -EFAULT;
        vm->clock_offset = (int64_t)(clock.clock - time_uptime_ns());
        return 0;
    }
    case KVM_SIGNAL_MSI:             return signal_msi(vm, argument);
    case KVM_IRQ_LINE:               return irq_line(vm, argument, 0);
    case KVM_IRQ_LINE_STATUS:        return irq_line(vm, argument, 1);
    case KVM_SET_GSI_ROUTING:        return set_gsi_routing(vm, argument);
    case KVM_IRQFD:                  return set_irqfd(vm, argument);
    case KVM_ENABLE_CAP:             return vm_enable_cap(vm, argument);
    case KVM_CREATE_IRQCHIP:         return create_irqchip(vm);
    case KVM_GET_IRQCHIP:            return irqchip_state(vm, argument, 0);
    case KVM_SET_IRQCHIP:            return irqchip_state(vm, argument, 1);
    case KVM_CREATE_PIT2:            return create_pit(vm, argument);
    case KVM_GET_PIT2:               return pit_state(vm, argument, 0);
    case KVM_SET_PIT2:               return pit_state(vm, argument, 1);
    case KVM_REINJECT_CONTROL:       return vm->pit_created ? 0 : -ENXIO;
    default:
        kprintf("kvm: VM ioctl 0x%x is not supported\n", (unsigned)request);
        return -ENOTTY;
    }
}

static void vm_release(struct file *file) {
    struct kvm_vm *vm = file->private_data;
    if (!vm) return;
    file->private_data = NULL;
    vm_put(vm);
}

static const struct vfs_file_ops vm_file_ops = {
    .release = vm_release,
    .ioctl = vm_ioctl,
};

static int64_t create_vm(uint64_t type) {
    if (type != 0) return -EINVAL;
    struct process *process = process_current();
    if (!process) return -EINVAL;
    struct kvm_vm *vm = kmalloc(sizeof(*vm));
    if (!vm) return -ENOMEM;
    memset(vm, 0, sizeof(*vm));
    mutex_init(&vm->lock, "kvm vm", LOCK_RANK_KVM_VM);
    mutex_init(&vm->irq_lock, "kvm irq", LOCK_RANK_KVM_IRQ);
    vm->refs = 1;
    vm->cr3 = process->cr3;
    vm->npt_root = alloc_zeroed_page();
    if (!vm->npt_root) {
        kfree(vm);
        return -ENOMEM;
    }
    int fd = install_fd(vm_node, vm);
    if (fd < 0) vm_put(vm);
    return fd;
}

// ---- /dev/kvm ----

static int64_t msr_index_list(uint64_t argument) {
    struct kvm_msr_list header;
    if (copy_from_user(&header, argument, sizeof(header)) != 0) return -EFAULT;
    uint32_t wanted = header.nmsrs;
    uint32_t count = sizeof(saved_msrs) / sizeof(saved_msrs[0]);
    header.nmsrs = count;
    if (copy_to_user(argument, &header, sizeof(header)) != 0) return -EFAULT;
    if (wanted < count) return -E2BIG;
    return copy_to_user(argument + sizeof(header), saved_msrs, sizeof(saved_msrs)) == 0 ? 0
                                                                                       : -EFAULT;
}

static int64_t supported_cpuid_ioctl(uint64_t argument) {
    struct kvm_cpuid2 header;
    if (copy_from_user(&header, argument, sizeof(header)) != 0) return -EFAULT;
    if (header.nent < 1) return -E2BIG;
    uint32_t capacity = header.nent > KVM_CPUID_MAX ? KVM_CPUID_MAX : header.nent;
    struct kvm_cpuid_entry2 *entries = kmalloc(capacity * sizeof(*entries));
    if (!entries) return -ENOMEM;
    uint32_t count = 0;
    int64_t status = supported_cpuid(entries, capacity, &count);
    if (!status) {
        header.nent = count;
        if (copy_to_user(argument, &header, sizeof(header)) != 0 ||
            copy_to_user(argument + sizeof(header), entries, count * sizeof(*entries)) != 0)
            status = -EFAULT;
    }
    kfree(entries);
    return status;
}

static int64_t kvm_device_ioctl(struct file *file, unsigned long request, uint64_t argument) {
    (void)file;
    switch (request) {
    case KVM_GET_API_VERSION:    return argument ? -EINVAL : KVM_API_VERSION;
    case KVM_CREATE_VM:          return create_vm(argument);
    case KVM_CHECK_EXTENSION:    return check_extension(argument);
    case KVM_GET_VCPU_MMAP_SIZE: return argument ? -EINVAL : (int64_t)(KVM_RUN_PAGES * 4096U);
    case KVM_GET_MSR_INDEX_LIST: return msr_index_list(argument);
    case KVM_GET_SUPPORTED_CPUID: return supported_cpuid_ioctl(argument);
    case KVM_GET_EMULATED_CPUID: {
        struct kvm_cpuid2 header;
        if (copy_from_user(&header, argument, sizeof(header)) != 0) return -EFAULT;
        header.nent = 0;
        return copy_to_user(argument, &header, sizeof(header)) == 0 ? 0 : -EFAULT;
    }
    case KVM_X86_GET_MCE_CAP_SUPPORTED: {
        uint64_t cap = MCG_CTL_P | MCG_SER_P | KVM_MCE_BANKS;
        return copy_to_user(argument, &cap, sizeof(cap)) == 0 ? 0 : -EFAULT;
    }
    default:
        kprintf("kvm: /dev/kvm ioctl 0x%x is not supported\n", (unsigned)request);
        return -ENOTTY;
    }
}

static const struct vfs_file_ops kvm_device_ops = {
    .ioctl = kvm_device_ioctl,
};

// ---- loading ----

static void msrpm_pass(uint8_t *map, uint32_t msr) {
    uint32_t base;
    if (msr < 0x2000) base = 0;
    else if (msr >= 0xC0000000U && msr < 0xC0002000U) base = 0x800 * 8, msr -= 0xC0000000U;
    else return;
    uint32_t bit = base + msr * 2;
    map[bit / 8] &= (uint8_t)~(3U << (bit % 8));
}

static int svm_available(void) {
    uint32_t a, b, c, d;
    cpu_cpuid(0x80000000U, 0, &a, &b, &c, &d);
    if (a < 0x8000000AU) return 0;
    cpu_cpuid(0x80000001U, 0, &a, &b, &c, &d);
    if (!(c & (1U << 2))) return 0;
    if (cpu_read_msr(MSR_VM_CR) & VM_CR_SVMDIS) {
        kprintf("kvm: SVM is disabled by the firmware\n");
        return 0;
    }
    cpu_cpuid(0x8000000AU, 0, &a, &b, &c, &d);
    if (!(d & 1U) || !(d & (1U << 3))) {
        kprintf("kvm: this processor's SVM lacks nested paging or the next RIP\n");
        return 0;
    }
    return 1;
}

static struct vfs_node *anonymous_node(const char *name, const struct vfs_file_ops *ops) {
    struct vfs_node *node = vfs_alloc_node(name, VFS_CHARDEVICE);
    if (!node) return NULL;
    node->mode = 0600;
    node->file_ops = ops;
    return node;
}

static int kvm_load(void) {
    if (!svm_available()) return -1;
    uint32_t a, b, c, d;
    cpu_cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1U << 27))) {
        kprintf("kvm: the guest FPU needs XSAVE\n");
        return -1;
    }
    host_xcr0 = xgetbv0();
    {
        uint8_t *area = kmalloc(PROCESS_FPU_STATE_SIZE + 64);
        if (!area) return -1;
        uint8_t *aligned = (uint8_t *)(((uintptr_t)area + 63U) & ~(uintptr_t)63U);
        __asm__ volatile("fxsave64 (%0)" : : "r"(aligned) : "memory");
        memcpy(&guest_mxcsr_mask, aligned + 28, 4);
        if (!guest_mxcsr_mask) guest_mxcsr_mask = 0xFFBF;
        kfree(area);
    }

    svm_cpu_slots = smp_cpu_count();
    svm_cpus = kmalloc(svm_cpu_slots * sizeof(*svm_cpus));
    if (!svm_cpus) return -1;
    memset(svm_cpus, 0, svm_cpu_slots * sizeof(*svm_cpus));
    for (unsigned index = 0; index < svm_cpu_slots; index++) {
        svm_cpus[index].hsave = alloc_zeroed_page();
        svm_cpus[index].host_save = alloc_zeroed_page();
        if (!svm_cpus[index].hsave || !svm_cpus[index].host_save) return -1;
    }

    // Every port and MSR exits, but the MSRs VMLOAD and VMSAVE switch with the guest.
    void *iopm = pmm_alloc_pages(3, 4096);
    void *msrpm = pmm_alloc_pages(2, 4096);
    if (!iopm || !msrpm) return -1;
    iopm_pa = (uint64_t)(uintptr_t)iopm;
    msrpm_pa = (uint64_t)(uintptr_t)msrpm;
    memset(vmm_phys_to_virt(iopm_pa), 0xFF, 3 * 4096);
    uint8_t *map = vmm_phys_to_virt(msrpm_pa);
    memset(map, 0xFF, 2 * 4096);
    static const uint32_t passed[] = {0x174,      0x175,      0x176,      0xC0000081, 0xC0000082,
                                      0xC0000083, 0xC0000084, 0xC0000100, 0xC0000101, 0xC0000102};
    for (unsigned index = 0; index < sizeof(passed) / sizeof(passed[0]); index++)
        msrpm_pass(map, passed[index]);

    vm_node = anonymous_node("kvm-vm", &vm_file_ops);
    vcpu_node = anonymous_node("kvm-vcpu", &vcpu_file_ops);
    if (!vm_node || !vcpu_node) return -1;
    vcpu_node->mmap = vcpu_mmap;

    struct vfs_node *dev = vfs_lookup("/dev");
    if (!dev) return -1;
    struct vfs_node *node = vfs_alloc_node("kvm", VFS_CHARDEVICE);
    if (!node) return -1;
    node->mode = 0666;
    node->dev_major = 10;
    node->dev_minor = 232;
    node->file_ops = &kvm_device_ops;
    if (vfs_attach(dev, node) != 0) {
        vfs_free_node(node);
        return -1;
    }
    kprintf("kvm: AMD SVM, %u processors\n", svm_cpu_slots);
    return 0;
}

// No exit: open VMs hold the processors' SVM state, so the module stays once loaded.
MODULE_MAIN(kvm_load, NULL);
MODULE_ALIAS("cpu:type:x86,ven*fam*mod*:feature:*00C2*");
MODULE_LICENSE("MIT");
MODULE_DESCRIPTION("KVM on AMD SVM");
MODULE_AUTHOR("Tunix");
