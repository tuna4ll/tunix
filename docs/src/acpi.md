# ACPI

The firmware describes the parts of a PC that are not on a bus with a standard
register layout -- the embedded controller, the lid, the brightness keys, the
thermal zones, the processors' performance limits -- in AML, a bytecode stored
in the DSDT and SSDTs. A kernel that does not run that code can only guess at
what each machine wants, one machine at a time. Tunix runs it.

## uACPI

The interpreter is [uACPI](https://github.com/uACPI/uACPI), MIT licensed. Its
recipe, `kernel/subprojects/uacpi/RECIPE`, pins `UACPI_VERSION` and lists the
sources the kernel uses. The kernel's own makefile includes it, so `make` in
`kernel/` clones uACPI into `kernel/build/uacpi` on the first build and compiles
it with the kernel's flags minus `-Werror`. Nothing of uACPI is kept in this tree.

uACPI needs a small host interface, `kernel/drivers/acpi/host.c`:

- **Memory.** Usable RAM is reached through the direct map; anything else --
  ACPI NVS, chipset registers -- gets an uncached device mapping, remembered so
  that an operation region mapped a thousand times costs one mapping.
- **Locks and events.** Mutexes and counting events sleep when a process is
  running and spin in `kmain`, where there is no process to put to sleep and
  `_INI` methods may still call `Sleep`.
- **Deferred work.** Two kernel threads. `kacpi-gpe` runs GPE methods and is
  pinned to CPU 0, which some firmware's SMI handlers assume. `kacpi-notify`
  runs `Notify` handlers anywhere.
- **The SCI.** A vector from `irq_request`, routed through the I/O APIC with
  the polarity and trigger the MADT gives.

## Bring-up

`acpi_subsystem_init` (`kernel/drivers/acpi/bus.c`) runs once timers work:

1. `uacpi_initialize` maps the tables and switches the firmware into ACPI mode.
2. `uacpi_namespace_load` builds the namespace from the DSDT and SSDTs.
3. The embedded controller's address space handler is installed, so the `_REG`
   and `_INI` methods that read it in the next step find it working.
4. `uacpi_namespace_initialize` runs `_STA` and `_INI`.
5. The drivers below attach, and the GPEs are enabled.

`acpi=off` on the command line skips all of it. The early table code in
`acpi.c` still reads the MADT for the processors and interrupt controllers,
which the kernel needs long before AML can run.

`_OSI` answers what Windows answers, as Linux does. Firmware is tested against
Windows, and on the machines this has run on that is the branch where the
brightness keys reach the operating system rather than a vendor tool.

## Drivers

**Embedded controller** (`ec.c`). Found by its `PNP0C09` id; the data and
command ports come from `_CRS`, the event's GPE from `_GPE`, and `_GLK` says
whether the firmware's global lock must be held. Reads and writes to the
`EmbeddedControl` region go through the standard 0x80/0x81 commands. When the
controller sets SCI_EVT its GPE fires, a 0x84 query returns the event number,
and the matching `_Qxx` method runs. A thread also polls SCI_EVT four times a
second, so an edge lost while the GPE was masked still gets drained.

**Video** (`video.c`). Every device with a `_BCM` method is a display output;
its notifications 0x86 and 0x87 are the brightness keys. They are reported as
`KEY_BRIGHTNESSUP` and `KEY_BRIGHTNESSDOWN` on the keyboard, and when no
program is reading the keyboard -- a text console -- the kernel steps
`/sys/class/backlight` itself. The parent bus is told through `_DOS` that the
operating system handles brightness. Firmware often notifies every output it
knows for one press, so a second identical key within 20 ms is dropped.

**Buttons** (`button.c`). The fixed power button from the FADT, control-method
power buttons (`PNP0C0C`) and lids (`PNP0C0D`).

**Thermal zones** (`thermal_zone.c`). Each zone's `_TMP`, `_PSV`, `_HOT` and
`_CRT` are read at start, `_TMP` every five seconds and on notification 0x80,
the trip points again on 0x81. Above `_PSV` every core runs at half speed
through the same clock modulation the processor's own sensor uses, until the
zone is 5 C under it; at `_CRT` the kernel flushes the disks and powers off.
The zones are `/sys/class/thermal/thermal_zoneN`.

**Processors** (`processor.c`). `_PDC` tells the firmware the kernel drives
P-states through IA32_PERF_CTL, which on many Intel machines is what makes the
firmware load the SSDTs holding `_PSS` and `_PPC`. `_PPC` is the highest state
the firmware currently allows, and notification 0x80 means it changed: the
embedded controller asks for a slower processor that way when it runs hot. The
limit is applied by `cpufreq` on each core's next tick.

## Testing without the hardware

QEMU has no embedded controller, so `make acpi-test` builds the drivers on the
host against uACPI and a model machine: I/O ports, an embedded controller that
answers queries from a queue, and physical memory holding an RSDP, an XSDT, a
FADT and a DSDT compiled by `iasl` from `tools/tests/acpi/machine.asl`. That
DSDT is shaped like a real laptop's: brightness keys that arrive as one query
with a code in controller RAM and are notified to two outputs, a throttle
request that changes `_PPC`, a thermal zone that reads the controller, a lid.
The test checks the whole path from a queued controller event to a key, a
processor limit, a cooling decision or a power-off.

The kernel threads are run one pass at a time, and `Sleep` advances a fake
clock, so the test is deterministic and takes milliseconds.
