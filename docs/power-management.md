# Power Management

Tunix can turn itself off and restart itself. `poweroff`, `reboot` and `halt`
work from a shell, and dinit runs them at the end of a shutdown the same way it
would on any other system.

## What the firmware has to be asked

There is no port that means "power down". The ports are named by the FADT, and
the *value* to write to them lives in the DSDT as an AML object, so getting a
machine to switch itself off needs both tables.

`acpi_describe_machine` (`src/kernel/acpi.c`) walks the RSDP to the RSDT or
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

`src/kernel/power.c` holds that sequence rather than the syscall, because the
power button's interrupt wants the same thing.

## The power button

The FADT names a global interrupt for the SCI, and `acpi_power_button_enable`
unmasks PWRBTN_EN in the PM1 event block and routes that interrupt to vector
0x30. The routing goes through `apic_route_global`, which takes polarity and
trigger from the MADT's overrides rather than from the ACPI default of
level-triggered active-low -- QEMU wires its own SCI active *high* and says so
in the table, so the default is not a safe guess.

On a press the kernel flushes and powers off. That is a policy decision made in
the absence of anywhere better to send it: there is no power-management daemon
here, and dinit's signals mean halt and reboot rather than power off. When
something exists to hand the event to, `power_button_pressed` is the one place
that has to change.

**This path is unverified.** QEMU's `pc` machine declares a fixed-feature power
button, everything the guest sets up reads back correct -- ACPI mode on,
PWRBTN_EN set, the PM block's timer counting so it is demonstrably the real
device -- and QEMU emits its own POWERDOWN event when asked, but PWRBTN_STS
never appears in the guest. The code follows what the tables ask for; a machine
that raises the event will be answered.

## Running it

`make run` no longer passes `-no-reboot -no-shutdown`. Those flags made QEMU
intercept the reset and the power-down, which from inside the guest looked like
`reboot` exiting the emulator and `poweroff` hanging. Pass
`QEMU_HALT_ON_RESET=1` to get them back while chasing a fault that resets the
machine.
