# KVM

Tunix runs virtual machines on AMD's SVM through Linux's `/dev/kvm`
interface, so QEMU, and anything else written against that interface,
accelerates its guests with the processor instead of emulating them.

```sh
qemu-system-x86_64 -enable-kvm -cpu host -m 2G ...
```

The module is `drivers/virt/kvm.c`. Like Linux's `kvm_amd`, it is loaded at
boot for the processor itself. Each `/sys/devices/system/cpu/cpuN` has
Linux's modalias for an x86 processor, which lists its features by Linux's
numbers, and the module's alias asks for feature `00C2`, which is SVM. It
works only on a processor with SVM,
nested paging and the next-RIP save (every AMD processor since the
Phenom II era has all three), and only where the firmware has not locked SVM
off. It is built for x86-64 only. Intel's VMX is not supported.

## The interface

`/dev/kvm` (character device 10:232) answers the system ioctls, and
`KVM_CREATE_VM` and `KVM_CREATE_VCPU` return file descriptors for a VM and its
vCPUs, as on Linux. The numbers and structure layouts are Linux's: they were
generated from `<linux/kvm.h>`, and static asserts in the module check every
size and offset against it. A vCPU's `kvm_run` is mapped from its descriptor,
with the data of port I/O in the page after it.

The capabilities advertised are the ones the module implements:

| Area | What there is |
| --- | --- |
| Memory | user memory slots, up to 256; a dirty log in which every page is dirty |
| vCPUs | up to 64; general, segment, FPU and XSAVE registers, XCR0, MSRs, debug registers, events, MP state, CPUID |
| Exits | port I/O, MMIO, HLT, an interrupt window, shutdown, failed entry, internal errors |
| Interrupts | the PICs, I/O APIC, local APICs and PIT in the kernel, or the local APICs alone; GSI routing, MSIs, irqfd; `KVM_INTERRUPT` and `KVM_NMI` |
| Other | ioeventfd, the VM clock, the TSC rate, machine-check banks, `immediate_exit` |

## The interrupt controller

QEMU's KVM on x86 needs the interrupt controller in the kernel, and it can have it
in either of the two ways Linux offers:

- `KVM_CREATE_IRQCHIP`, QEMU's default: two 8259 PICs, an I/O APIC at
  `0xFEC00000` and a local APIC per vCPU, with `KVM_CREATE_PIT2` adding the 8254
  timer on ports 0x40-0x43 and 0x61.
- `KVM_CAP_SPLIT_IRQCHIP` (`kernel-irqchip=split`): the local APICs only. The
  PIC's interrupt comes in with `KVM_INTERRUPT`, the I/O APIC's as MSIs, and the
  EOI of a level-triggered interrupt goes out as `KVM_EXIT_IOAPIC_EOI`.

The chips behave as Linux's emulation of them does (`arch/x86/kvm/i8259.c`,
`ioapic.c`, `lapic.c`, `i8254.c`). Their state is under the VM's `irq_lock`.

The local APIC sits at its APIC base. A guest access there is decoded like any
MMIO and answered in the kernel. Its parts:

- the TPR, shared with CR8 through `V_TPR`;
- the ISR, IRR and TMR, and EOI;
- the one-shot and periodic timer, counting at 1 GHz as KVM's does;
- IPIs in physical and logical (flat and cluster) mode, with INIT and SIPI.

Application processors wait in the kernel for INIT and SIPI, and a halted vCPU
waits there for an interrupt it would take. The interrupt can come from:

- its APIC timer or the PIT, whose deadlines bound the wait;
- an IPI or an MSI (`KVM_SIGNAL_MSI`, or a route of `KVM_SET_GSI_ROUTING`);
- an irqfd: an eventfd whose writes the kernel turns into the GSI's
  interrupt, through a consumer hook in `ipc/eventfd.c`;
- `KVM_IRQ_LINE`.

A vCPU running a guest is made to exit by a reschedule IPI to its processor.
Interrupts are injected with `EVENTINJ` when the guest can take them. When it
cannot, a virtual interrupt it never sees is set up with the pending vector's
priority, so the processor itself exits when the guest's IF and TPR let it in.
An NMI blocks the next one until the handler's IRET has completed, which an
IRET intercept tracks.

## Entering a guest

Each vCPU has a VMCB. `KVM_RUN` loops: it completes what userspace was asked
to do (the data of an `IN`, or of an MMIO load), queues the event to inject,
runs the guest with `VMRUN`, and handles the exit. Exits the kernel can finish
by itself, such as CPUID or an MSR access, go straight back into the guest.
Port I/O and MMIO that are not the in-kernel chips' return to userspace, as
`HLT` does when there is no in-kernel APIC. A pending signal makes
`KVM_RUN` return `-EINTR`, and so does `immediate_exit`, which is how QEMU
kicks a vCPU thread.

`VMRUN` is entered with interrupts masked by GIF from before the guest's FPU
state is loaded until after the host's is back. A host interrupt while the
guest runs makes it exit (`V_INTR_MASKING`), and the interrupt is taken as soon
as GIF is set again. The kernel otherwise runs with interrupts off, so after
an interrupt exit it opens them for one instruction to take that interrupt;
without that, it stays pending and ends every `VMRUN` at once. The host's FS, GS, TR, LDTR and system-call MSRs are saved
with `VMSAVE` around the run. The guest's own values of the same MSRs are in its
VMCB and reach the guest without exits. Every other MSR, and every port, exits.

All vCPUs use ASID 1. A processor flushes the guest TLB whenever it runs a
different vCPU than last time, or when the nested page tables have changed
since this vCPU last entered.

## Guest memory

Guest physical memory is mapped by nested page tables, which start empty.
When a guest first touches a page, the nested page fault finds the slot
behind it, pins the userspace page (breaking copy-on-write first, so it is
the page the VMM writes too) and maps it, and up to fifteen of its neighbours
in the same slot. A fault on an address no slot covers is MMIO.

When a slot is removed, its entries are cleared first, and every vCPU is
waited for until it is either out of the guest or has entered again with the
new tables. Only then are the pages unpinned. A guest can never write to a page
after it has been given back.

Pinned pages are not swapped out: the swap cache keeps a page whose reference
count says someone else holds it, so the VMM and the guest keep seeing the
same memory.

## MMIO

An MMIO exit needs the instruction: its width, which register, how long it is.
The module walks the guest's page tables (none, 32-bit, PAE or four-level) to
fetch it, and decodes the forms drivers and firmware use for device registers:
`MOV` in all its encodings, `MOVZX`/`MOVSX`, and the string instructions
`MOVS`, `STOS` and `LODS`, with `REP` going round once per exit. An
instruction outside these ends `KVM_RUN` with `KVM_EXIT_INTERNAL_ERROR` and a
line in the kernel log, rather than being guessed at.

String port I/O (`INS`, `OUTS`) moves as many elements per exit as fit in the
guest page and in the data page, which is what makes PIO disk reads tolerable.

## CPUID and MSRs

`KVM_GET_SUPPORTED_CPUID` is the host's CPUID less what the vCPU does not
give a guest:
- SVM and VMX themselves, and MONITOR/MWAIT;
- performance counters and IBS;
- the x2APIC and TSC-deadline timer, which belong to an in-kernel APIC;
- speculation controls, RDTSCP and RDPID;
- SEV, and XSAVES.

The guest's CPUID comes from `KVM_SET_CPUID2`, with OSXSAVE and the XSAVE area
size following the guest's CR4 and XCR0.

The MSRs a guest can use are the ones in its VMCB, plus these kept per vCPU:
- the TSC (as an offset) and the APIC base;
- PAT, the MTRRs, and the machine-check banks `KVM_X86_SETUP_MCE` asks for;
- `MISC_ENABLE` and `HWCR`.

Any other MSR gives the guest a #GP, as Linux's KVM does by default.

## What is not here

- Intel VMX.
- The x2APIC, the TSC-deadline timer, AVIC, and resampling irqfds.
- Dirty logging that tracks writes, so live migration copies everything.
- Huge pages in the nested page tables.
- Guest debugging (`KVM_SET_GUEST_DEBUG` with breakpoints), SMM, and nested
  virtualization inside the guest.
