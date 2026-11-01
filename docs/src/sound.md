# Sound

Tunix speaks ALSA's kernel interface, the same one Linux exposes under
`/dev/snd`, so Void's own alsa-lib, alsa-utils, OpenAL Soft and Firefox run on
it unchanged. The stack, from a game down to the hardware:

```
SuperTuxKart -> OpenAL Soft -> alsa-lib "default":  plug -> softvol -> dmix
                                                                       |
            /dev/snd/pcmC0D0p, controlC0, timer  <---------------------+
                                  |
            kernel/drivers/audio/sound.c   (PCM, control and timer devices)
                                  |
            kernel/modules/snd_hda.c       (Intel HD Audio controller and codec)
```

`default` is alsa-lib's stock configuration for an HD Audio card: dmix mixes
every program into one stream, softvol adds the `PCM` volume, and plug converts
whatever a program asks for. Several programs play at once, as on any Linux
machine without a sound server. Nothing in `/etc` overrides it.

## Devices

Each card registered by a driver gets Linux's names and numbers (major 116):

| node                 | minor           | what                               |
|----------------------|-----------------|------------------------------------|
| `controlC<n>`        | `32n`           | mixer controls and card information |
| `pcmC<n>D0p`         | `32n + 16`      | playback                           |
| `pcmC<n>D0c`         | `32n + 24`      | capture                            |
| `timer`              | 33              | ALSA timers, one per PCM stream    |

Up to four cards; `/proc/asound` has `cards`, `devices`, `pcm`, `version` and
per card `id`, `codec#0` (the driver's view of the codec, below),
`pcm0p/info` and `pcm0p/sub0/{status,hw_params,sw_params}` (capture likewise).

In sysfs a card sits below its PCI device the way Linux puts it,
`/sys/devices/pci0000:00/0000:00:1b.0/sound/card0` with `number` and `id`, and
the control and PCM devices below the card, each with a `device` link back up.
udev's `78-sound-card.rules` relies on that shape: when `controlC0` appears it
writes `change` into the card's `uevent`, and the card's change event is what
marks it `SOUND_INITIALIZED`, the property PipeWire waits for. The nodes, the
sysfs devices and the udev events appear when a driver registers a card and
disappear when it unregisters.

## The PCM core

`sound.c` holds no driver knowledge: a driver describes what its hardware can
do (rates, formats, valid bits, channel and period limits) and supplies five
callbacks (`configure`, `prepare`, `trigger`, `position`, `release`). The core
does everything an application sees:

- **Parameter refinement** with Linux's interval arithmetic: open and closed
  bounds, integer intervals, and the same rules tying sample bits, frame bits,
  channels, rate, period and buffer sizes, times and byte counts together, run
  to a fixed point. `HW_PARAMS` chooses the remaining free values the way
  Linux does (first of everything, largest buffer). OpenAL asking for
  1024 x 8 frames gets 1024 x 8 frames.
- **Transfers**: blocking `WRITEI`/`READI` and `write()`/`read()` move every
  frame, sleeping as needed; non-blocking ones move what fits and then answer
  `EAGAIN`. `poll` reports room or data against `avail_min`.
- **States**: prepare, start (playback starts on its own at `start_threshold`),
  drop, drain (sleeps until the queue has played; non-blocking answers
  `EAGAIN`), pause and resume, rewind and forward, reset, and underruns or
  overruns at `stop_threshold`, which a following call reports as `EPIPE`.
- **mmap** of the ring and `SYNC_PTR`, which is how alsa-lib drives it; an mmap
  commit does not start the stream, exactly as on Linux.
- **Silence**: with `silence_size` at the boundary (dmix sets that) the core
  zeroes every frame the hardware has played, so a stream that never stops
  plays silence rather than old audio; the threshold mode fills ahead of the
  application pointer. Nothing is filled unless asked for.
- **The hardware pointer** is read from the driver whenever anything looks at
  the stream and at every timer tick. Positions that the elapsed time cannot
  explain (QEMU's controller steps a few frames back when it resumes) are
  ignored, and a pointer that wrapped the whole ring while nobody looked is
  caught from the elapsed time.
- **Shared opens**: a second `open` of a busy stream fails with `EBUSY`, unless
  it says `O_APPEND`, in which case it joins the stream already open. That is
  how dmix's secondary clients reach the card.

Per-open state lives in `struct file`'s `private_data`, which character
devices can now keep through `struct vfs_file_ops` (open, release, read,
write, poll, ioctl).

## Timers

dmix and dsnoop sleep on an ALSA timer that ticks once per hardware period of
the stream they share, not on the PCM device. `/dev/snd/timer` offers one
PCM-class timer per stream (`hw:CLASS=3,SCLASS=0,CARD=n,DEV=0,SUBDEV=0` for
playback, `SUBDEV=1` for capture) with Linux's ioctls: version, next device,
select, info, params (ticks, event filter, early event), status, start, stop,
pause and continue. Reads return timestamped events (`TREAD`) or plain tick
counts. Starting, stopping and pausing the PCM stream are reported to the
timer's readers as well.

## Controls

A driver lists its controls (name, interface, type, count, range, enumerated
items, a dB scale) and answers get and put. The core numbers them, answers
`ELEM_LIST`, `ELEM_INFO`, `ELEM_READ`, `ELEM_WRITE` and `TLV_READ` (the dB scale
that makes `amixer` and PulseAudio-style mixers show decibels), and queues
events for every subscriber: a value change from any opener or from the driver
(a headphone plugged in) wakes `poll` on the control device and is read as
`struct snd_ctl_event`.

Programs can add controls of their own (`ELEM_ADD`, `ELEM_REPLACE`,
`ELEM_REMOVE`, `TLV_WRITE`), integer, boolean or enumerated, up to 32 per card.
softvol does that to create `PCM Playback Volume`; the control lasts as long as
the card.

## The HD Audio driver

`snd_hda.ko` binds every HD Audio controller (up to four). For each it resets
the link, starts the command rings, and parses the first codec with an analog
output:

- **Outputs**: every connected line-out, speaker and headphone pin, in the
  order the firmware's pin configuration gives them, each routed to its own
  converter where the codec has enough of them. Mixers on the way are set to
  pass the converter and mute everything else (the analog loopback inputs are
  a common source of hiss). EAPD, the external amplifier enable most laptops
  need for their speakers, is switched on for every pin that has it.
- **Inputs**: microphone jacks, internal microphones and line inputs, routed to
  the first converter that reaches them, with the 80% reference voltage
  microphones expect.
- **Formats and rates** come from the converters' own capabilities: 16-bit
  where that is all a codec does (QEMU's), and 32-bit samples with 20 or 24
  valid bits where the codec has those, programmed with the matching width.
- **Jacks**: pins that can sense a plug are polled four times a second by the
  sound core's `ksoundd` thread. Plugging headphones mutes the speakers and
  line outs (`Auto-Mute Mode`), plugging a microphone switches capture to it,
  and either change is an event on the control device.
- **Interrupts**: one per period through MSI, both arches, with the timer tick
  as the fallback where MSI is missing.
- **Controllers that only drive HDMI or DisplayPort** (the NVIDIA one next to
  the laptop's GPU) are left alone.
- On Intel controllers the traffic class is cleared and PCI Express no-snoop is
  turned off, as Linux does, so audio DMA always sees what the CPU wrote.

`/proc/asound/card<n>/codec#0` lists the codec, every widget with its
capabilities, amplifiers, pin configuration, current pin control, EAPD and
plug state, and connections (the selected one starred), followed by the paths
the driver chose, for example
`Speaker: 0x02 -> 0x0c[0] -> 0x14[0]`. That is the first thing to read when a
machine stays silent.

Controls, with Linux's names: `Master Playback Volume` and `Switch` (with the
codec's dB scale), one `Playback Switch` per output (`Speaker`, `Headphone`,
`Line Out`), `Auto-Mute Mode`, `Capture Volume` and `Switch`, `Input Source`
when there is a choice to make, `Mic Boost Volume` and friends, and a `Jack`
control per pin that can sense (a `Phantom Jack` for the ones that cannot).

## Userland

- **alsa-utils is on the image.** udev runs `alsactl restore` when a control
  device appears, which on a first boot initialises the card the way Linux
  distributions do (Master at -20 dB), and the `alsa` service stores the mixer
  state at shutdown. `aplay -l`, `amixer`, `alsamixer`, `speaker-test` and
  `arecord` work.
- **dmix needs System V semaphores**, which the kernel now has (`semget`,
  `semop`, `semtimedop`, `semctl` with `SEM_UNDO`, timeouts, removal waking the
  waiters with `EIDRM`); see `kernel/ipc/sysvsem.c`.
- `/etc/openal/alsoft.conf` asks for four periods of 1024 frames, 85 ms. Under
  QEMU with the game rendering in software on four CPUs, 512 x 3 (32 ms)
  underran once in a 108 second race, 1024 x 4 never did. The old setting,
  1024 x 8, was needed only because the driver was wrong (below).

## PipeWire

The images do not run a sound server (Weston has none installed; the GNOME and
KDE images carry PipeWire but do not start it), but the kernel side has been
checked against one: a small Void root with `pipewire`, `wireplumber`,
`pipewire-pulse`, `alsa-pipewire` and eudev, booted under QEMU. WirePlumber
finds the card through udev and builds `Built-in Audio Analog Stereo` from
alsa-card-profiles, `pactl` talks to pipewire-pulse, and a program playing
through ALSA's `pipewire` plugin reaches the card intact (the frame counter
arrived without a gap over six seconds, `xruns: 0`).

Two things it needed beyond ALSA itself: `send()` and `recv()` on a descriptor
that is not a socket must fail with `ENOTSOCK`, not `EBADF` (libpulse writes to
its wakeup pipe with `send()` and falls back to `write()` only on `ENOTSOCK`),
and unix sockets accept `SO_PRIORITY` and the buffer-size options.

Under QEMU, PipeWire wants some headroom (`api.alsa.headroom = 1024`): without
it the first tenth of a second after starting can underrun once, because the
emulated controller's position moves in steps. Linux's PipeWire applies that by
itself when DMI says the machine is a VM; Tunix does not publish DMI yet, so a
WirePlumber rule would have to say it. Real controllers move smoothly and need
none.

## What was wrong before 2026-10-25

Measured by playing a known signal (a 1 kHz sine on the left, a frame counter
on the right) through the same path the game uses, recording QEMU's output to a
WAV file and checking every frame:

- **The stream played at 70% of real time.** The driver offered 32-bit samples
  without asking the codec, OpenAL took them, and QEMU's codec, which only does
  16-bit, consumed the ring at the wrong rate. Six seconds of OpenAL became 4.2
  seconds of recording; through PulseAudio on the host that is constant
  stutter. On real codecs, which mostly stop at 24 bits, it was a gamble.
- **Parameter refinement forced every interval to be closed and integer**, so
  alsa-lib's "nearest" searches went astray: OpenAL asked for 1024 x 8 and got
  1008 x 3.
- **A blocking write returned after a fraction of its frames**, which Linux
  never does. Programs that do not loop on short writes lost the rest:
  measured, 49 frames out of every 1056.
- **Silence was written ahead of the application** whenever half the ring was
  free, over audio OpenAL was in the middle of rendering into the mmap'd ring.
- Drain spun on the CPU with the sound lock held for up to 300 ms, pause was
  refused, there was no capture, one card, one program, and two mixer
  controls without a dB scale.

All of that measured clean after the rewrite: the counter arrives without a
single missing, repeated or zeroed frame, at real time, through `hw`, through
`plug` and through dmix with two programs playing at once (each channel a
clean sine, worst residual 2 of 12000).

SuperTuxKart in the Weston desktop, three-lap race recorded through QEMU's
`wav` backend: 122 seconds of recording for 122 seconds of wall time, no gap of
digital silence as short as 5 ms inside the race, no clipping, `xruns: 0` in
`/proc/asound/card0/pcm0p/sub0/status`, and no underrun in OpenAL's log.

## Tests

`make hda-test` runs the driver's codec parser on the host against simulated
codecs: a laptop Realtek ALC269 (internal speaker and headphone jack on their
own DACs, microphone jack and internal microphone behind a mixer), a desktop
codec with a capture selector and front and rear microphones, QEMU's codec and
an HDMI-only one. It checks the routes, the formats and rates, the controls and
their dB scales, unplugged and plugged headphones (the speaker is muted and
comes back), microphone switching and the codec dump: the parts of the driver
QEMU's codecs, which have one pin and no jack sensing, cannot exercise.

`make test` runs `sound`: QEMU gets two HD Audio controllers, one playing into a
WAV file and one with a codec that has a line input, and the test loads
`snd_hda.ko` itself. It checks card information, controls, dB scales, events,
user controls, refinement (period and buffer times near what OpenAL asks for),
a two-second blocking write at the hardware rate, status and `/proc/asound`,
drain, underrun and recovery, pause and resume, rewind and forward,
non-blocking writes and `poll`, mmap with `SYNC_PTR`, `O_APPEND` joining, the
PCM timer ticking once per period, and a one-second blocking capture at the
hardware rate on the second card. Afterwards `wavcheck.sh` reads the recording
on the host and fails the test unless the reference tone's frame counter
arrived in one unbroken run. `ipc` covers the semaphores.

## Where the sound comes out

`make run` looks for a PulseAudio socket, at `/mnt/wslg/PulseServer` under WSL
or in `XDG_RUNTIME_DIR` on a Linux desktop, and plays there with a tenth of a
second of latency, which WSL's RDP audio channel needs; without one the machine
is silent (`-audiodev none`).
