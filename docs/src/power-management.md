# Power Management

Tunix can turn itself off and restart itself. `poweroff`, `reboot` and `halt`
work from a shell, and runit runs them at the end of a shutdown the same way it
would on any other system.

## What the firmware has to be asked

There is no port that means "power down". The ports are named by the FADT, and
the *value* to write to them lives in the DSDT as an AML object, so getting a
machine to switch itself off needs both tables.

`acpi_describe_machine` (`kernel/drivers/acpi/acpi.c`) walks the RSDP to the RSDT or
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

## \_S5_

With the [ACPI](acpi.md) subsystem running, S5 is entered the way the firmware
expects: `_PTS` and `_GTS` run, then uACPI writes the sleep type from `_S5` to
the PM1 control blocks.

The early table code keeps a fallback for machines where AML never came up --
`acpi=off`, or a firmware uACPI rejects. `parse_sleep_state` scans the DSDT for
the four characters `_S5_` followed by a package opcode and decodes the one or
two small integers inside it. Names in AML are always four characters with
short ones padded, which is why the object the source calls `_S5` is `_S5_` in
the table. A false match is rejected by the package opcode that has to follow
it.

## Turning it off

`acpi_power_off` asks the ACPI subsystem first. Without it, it writes
`SLP_TYP << 10 | SLP_EN` to PM1a_CNT, and to PM1b_CNT where the machine has
one, after `acpi_enable` has written the FADT's enable value to the SMI command
port and waited for SCI_EN -- until the firmware hands the fixed hardware over,
the sleep registers are the firmware's.

## Restarting

`acpi_reset` tries, in order, the ACPI subsystem's reset, the FADT's reset
register when the table says it has one (a port or a memory address), the
keyboard controller's reset line, which predates ACPI by a decade and is still
wired on every PC, and finally a triple fault: an empty interrupt table means
the processor cannot deliver a breakpoint, cannot deliver the double fault
that follows, and resets.

## reboot(2)

`sys_reboot` (syscall 169) is what userspace calls. It requires an effective uid
of zero and the two magic numbers Linux requires -- the call takes the machine
away, so a wild syscall with plausible arguments must not be able to reach it.

| command | what happens |
| --- | --- |
| `RB_POWER_OFF` | flush, then S5 |
| `RB_AUTOBOOT` | flush, then reset |
| `RB_HALT_SYSTEM` | flush, then stop |
| `RB_ENABLE_CAD` / `RB_DISABLE_CAD` | accepted; they concern Ctrl+Alt+Del only |

The flush is `ext2fs_sync` and an ATA cache flush. File writes reach ext2 as
they happen, so this is metadata and the drive's own cache rather than a
writeback cache of file contents, and it finishes in milliseconds.

`kernel/core/power.c` holds that sequence rather than the syscall, because the
power button's interrupt wants the same thing.

## The power button

As on Linux, a press is reported as `KEY_POWER` from an input device of its own,
`/dev/input/event3`, named "Power Button" and tagged `ID_INPUT_KEY` so that
udev marks it as a power switch. The ACPI subsystem raises it from the FADT's
fixed power button and from control-method buttons (`PNP0C0C`).

What happens next is userspace's decision, as it is on Linux. elogind powers
off, or leaves it to GNOME when GNOME holds its inhibitor; the GNOME image sets
GNOME's action to the shutdown dialog, since Tunix has no suspend to fall back
to. The Weston image runs acpid, whose handler runs `shutdown`. When nothing has
the device open -- a shell as init, the test and report images -- the kernel
flushes the disks and powers off itself, from a worker rather than the
interrupt.

`RB_DISABLE_CAD`, which runit sends at boot, used to take the button away from
the kernel. It is about Ctrl+Alt+Del and no longer touches the button.

## Temperature

Two things watch the temperature.

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

The firmware's thermal zones are the other: above a zone's `_PSV` every core
is slowed the same way, at `_CRT` the machine powers off, as described in
[ACPI](acpi.md). The firmware's trip points are usually set for the whole
machine -- the case, the battery -- rather than only the processor die.

The processor readings are `/sys/class/hwmon/hwmon0` in Linux's coretemp
layout, the zones `/sys/class/thermal/thermal_zoneN`. The `hwreport` boot
prints both and starts a logger in `rc.local` that appends uptime, load, every
core's temperature and the three busiest processes to `/tunix-thermal.log`
every five seconds and syncs it, so a machine that switches itself off leaves
the minutes before it on the disk.

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

The firmware can lower that ceiling. When a processor object's `_PPC` names a
slower state from its `_PSS` table -- an embedded controller does this when the
machine runs hot -- each core's next tick asks for that state's ratio instead,
and the full ratio again once `_PPC` is back to zero.

Once a second each core also reads APERF and MPERF, whose ratio is the speed it
actually ran at while it was busy. Both show up in
`/sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq`, in the `hwreport`
boot's `frequency` section, and in `/tunix-thermal.log`.

## Running it

`make run` no longer passes `-no-reboot -no-shutdown`. Those flags made QEMU
intercept the reset and the power-down, which from inside the guest looked like
`reboot` exiting the emulator and `poweroff` hanging. Pass
`QEMU_HALT_ON_RESET=1` to get them back while chasing a fault that resets the
machine.
