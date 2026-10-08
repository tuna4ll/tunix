# Power Management

Tunix can turn itself off and restart itself. `poweroff`, `reboot` and `halt`
work from a shell, and runit runs them at the end of a shutdown the same way it
would on any other system.

## What the firmware has to be asked

There is no port that means "power down". The ports are named by the FADT, and
the *value* to write to them lives in the DSDT as an AML object, so getting a
machine to switch itself off needs both tables.

`acpi_describe_machine` (`kernel/drivers/acpi.c`) walks the RSDP to the RSDT or
XSDT and reads two tables from it:

- the **MADT**, which describes the processors and the interrupt controllers.
  This is what the SMP and APIC code has always used.
- the **FADT**, which names the SMI command port, the PM1 control and event
  blocks, the SCI's global interrupt number and, on machines that have one, a
  reset register.

The FADT has grown four times and every version left the older fields where
they were, so a short table is an old one rather than a broken one. Every field
is read through an accessor that answers zero past the table's own length,
which is what makes ignoring the difference safe.

## \_S5_, and the small amount of AML this reads

The FADT points at the DSDT, which is AML bytecode. Interpreting it properly
means a bytecode machine with a namespace and operation-region drivers -- a
subsystem, not a function -- and nothing else here needs one.

What `parse_sleep_state` does instead is scan the DSDT for the four characters
`_S5_` followed by a package opcode, and decode the one or two small integers
inside it. Those are the SLP_TYP values for the two PM1 blocks. Names in AML
are always four characters with short ones padded, which is why the object the
source calls `_S5` is `_S5_` in the table.

A false match is rejected by the package opcode that has to follow it. If one
somehow got through, the result would be a machine that declined to power off,
not one that did something unexpected.

## Turning it off

`acpi_power_off` writes `SLP_TYP << 10 | SLP_EN` to PM1a_CNT, and to PM1b_CNT
where the machine has one. Before that it calls `acpi_enable`, which writes the
FADT's enable value to the SMI command port and waits for SCI_EN to appear --
firmware hands the fixed hardware over on request, and until it does the sleep
registers are the firmware's. A machine that boots through UEFI usually arrives
with ACPI mode already on, which `acpi_enable` notices and leaves alone.

## Restarting

`acpi_reset` tries three things in order:

1. the FADT's reset register, when the table says it has one. It can be a port
   or a memory address; both are handled.
2. the keyboard controller's reset line, which predates ACPI by a decade and is
   still wired on every PC. QEMU's `pc` machine advertises no reset register, so
   this is the one that actually runs there.
3. a triple fault. An empty interrupt table means the processor cannot deliver
   a breakpoint, cannot deliver the double fault that follows, and resets.

## reboot(2)

`sys_reboot` (syscall 169) is what userspace calls. It requires an effective uid
of zero and the two magic numbers Linux requires -- the call takes the machine
away, so a wild syscall with plausible arguments must not be able to reach it.

| command | what happens |
| --- | --- |
| `RB_POWER_OFF` | flush, then S5 |
| `RB_AUTOBOOT` | flush, then reset |
| `RB_HALT_SYSTEM` | flush, then stop |
| `RB_ENABLE_CAD` / `RB_DISABLE_CAD` | hand the power button to the kernel, or away from it |

The flush is `ext2fs_sync` and an ATA cache flush. File writes reach ext2 as
they happen, so this is metadata and the drive's own cache rather than a
writeback cache of file contents, and it finishes in milliseconds.

`kernel/power.c` holds that sequence rather than the syscall, because the
power button's interrupt wants the same thing.

## The power button

The FADT names a global interrupt for the SCI, and `acpi_power_button_enable`
routes it to vector 0x30 with polarity and trigger from the MADT's overrides --
QEMU wires its SCI active *high* and says so, so the ACPI default of
level-triggered active-low is not a safe guess.

Taking the SCI means taking the machine out of legacy mode, and that hands the
OS more than a button. On a laptop the firmware's embedded controller raises
general-purpose events for temperature, fans, the lid and the battery, and in
ACPI mode it waits for the OS to answer them by running AML. Tunix runs none.
So the kernel decides first:

- If the DSDT or an SSDT declares an embedded controller (`PNP0C09`) and the
  firmware booted in legacy mode, the kernel leaves it there. The firmware keeps
  doing what it did before any OS ran, and the button is the firmware's.
- Otherwise it enables ACPI mode, turns off every PM1 fixed event but
  PWRBTN_EN, clears their status, and writes zero to every GPE enable register:
  an event nobody handles must not hold a level-triggered SCI asserted. A GPE
  that fires anyway is cleared and masked by the interrupt and counted.
- `acpi_button=on` or `acpi_button=off` on the command line overrides that.

A press queues the power-off on a kernel worker. Flushing the disks sleeps on
locks and on I/O, which an interrupt cannot do.

`support/tests/acpi-kerneltest.sh` boots q35 four times -- plain, with an SSDT
declaring an embedded controller, and with each override -- presses the button
through QMP, and checks the decision, the press and whether the machine went
off.

## Temperature

On Intel processors with a digital thermal sensor (CPUID 06H:EAX[0] and the
ACPI thermal MSRs, 01H:EDX[22]) each core reads its own IA32_THERM_STATUS once
a second from its timer tick. The limit is TjMax from MSR_TEMPERATURE_TARGET on
the models that have it, 100 C otherwise.

- Within 5 C of the limit the core runs at half speed through
  IA32_CLOCK_MODULATION, until it is 15 C below again.
- Within 1 C of the limit, or with the critical-temperature bit set, for three
  seconds running, the kernel logs it and powers off through the same worker
  as the button: flushed, rather than cut by the hardware with the disk caches
  still in memory.

The readings are `/sys/class/hwmon/hwmon0` in Linux's coretemp layout, and the
`hwreport` boot prints them with the ACPI decision. That boot also starts a
logger in `rc.local` that appends uptime, load, every core's temperature and
the three busiest processes to `/tunix-thermal.log` every five seconds and
syncs it, so a machine that switches itself off leaves the minutes before it
on the disk. `make thermaltest` runs the thermal code on the host against a
modelled sensor.

## Speed

Nothing above the kernel picks a P-state here, so on Intel processors with
Enhanced SpeedStep the kernel does: each core's first timer tick reads the
ratio the firmware left it at and, when that is lower, asks IA32_PERF_CTL for
the highest ratio from MSR_PLATFORM_INFO -- one above it when the processor has
turbo and the firmware has not disabled it, which lets the hardware turbo. A
firmware that boots at the lowest ratio would otherwise leave every program at
half speed for as long as the machine runs; a firmware that already asked for
turbo, as the first laptop this ran on does, is left alone.
Nehalem and Westmere take the ratio in bits 7:0 of the register, Sandy Bridge
and later in bits 15:8; Atoms, older parts and machines whose firmware turned
SpeedStep off are left alone.

Once a second each core also reads APERF and MPERF, whose ratio is the speed it
actually ran at while it was busy. Both show up in
`/sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq`, in the `hwreport`
boot's `frequency` section, and in `/tunix-thermal.log`. `make thermaltest`
also runs this code on the host against modelled registers.

## Running it

`make run` no longer passes `-no-reboot -no-shutdown`. Those flags made QEMU
intercept the reset and the power-down, which from inside the guest looked like
`reboot` exiting the emulator and `poweroff` hanging. Pass
`QEMU_HALT_ON_RESET=1` to get them back while chasing a fault that resets the
machine.
