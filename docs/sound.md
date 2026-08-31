# Sound

Tunix has one sound card: the Intel HD Audio controller, behind ALSA's
`/dev/snd` interface. Void's own alsa-lib opens it, and SuperTuxKart plays
through it by way of OpenAL Soft.

```
game -> OpenAL Soft -> alsa-lib -> /dev/snd/pcmC0D0p -> hda.c -> the controller
```

The driver was written and verified in 2026-08 against programs built here.
Everything below is what it took to make Void's stack, which had never touched
it, work.

## Four things stood between a working driver and a sound

**The device nodes were in a group nobody was in.** `/dev/snd` is created
`0660 root:audio`, and the kernel had audio as group 29, which is Debian's
number. On this image 29 names no group at all and `audio` is 12. A node in a
group nobody is in is a node only root can open, and nothing says so: a program
reports that the machine has no sound card. `devnum.h` now carries Void's
numbers, and they have to be checked against the image's `/etc/group` if the
userland is ever changed underneath.

**The session is given exactly the groups it is named.** `chpst -u
tunix:tunix:_seatd` hands over those two groups and no others, whatever
`/etc/group` says, so the desktop could not reach the card even after the
numbers were right. The service runs `chpst -u tunix:tunix:_seatd:audio` now.
This is worth remembering because a login shell behaves differently: `login`
reads `/etc/group` and grants everything there, so the same command works by
hand and fails in the session.

**A refine has to say what it changed.** `SNDRV_PCM_IOCTL_HW_REFINE` answers
with the parameters narrowed to what the hardware can do, and `cmask` says
which of them moved. The driver answered `cmask = rmask`, meaning "everything
you asked about changed". Opening the card directly survives that, because the
`hw` plugin takes the answer and asks nothing more. A plugin chain does not:
alsa-lib refines the slave, maps whatever `cmask` names back up the chain, and
repeats until nothing changes. Told that everything changed, it re-derives the
client parameters from a slave that never moved and gives up in the library, so
`snd_pcm_hw_params` returns EINVAL with no ioctl behind it. `aplay -D hw:0,0`
worked; `aplay -D default` did not, and neither did anything else going through
`plug`, which is nearly every program.

**A buffer is a whole number of periods.** Asked for a 528 frame period and a
1536 frame buffer, the driver answered that periods lay between 2 and 3, which
is true and useless: there is no such configuration. alsa-lib believed it, and
found out only when it committed. Refusing the impossible set is what sends the
caller back to ask for 1584 frames, which is what OpenAL does, and then the
whole thing works.

## What the game needed on top

OpenAL Soft asks for a 33 millisecond buffer: three periods of 512 frames, and
it raises its mixing thread's priority so it can keep one that small. Both
halves of that mattered here.

The kernel had one priority level, so `pthread_setschedparam` was refused and
the mixer ran level with the game drawing a hundred frames a second beside it.
The stream underran continuously: `mmap commit error: Broken pipe`, over and
over, and silence. It has priorities now (see
[Syscalls and Scheduler](syscalls-and-scheduler.md)), and with the same image
and the same emulator, only the kernel differing, a run of the game went from
**322 underruns to none**.

`/etc/openal/alsoft.conf` still asks for 8192 frames, which is 170
milliseconds, because the priority is not the whole story: 512 frames times 4
was tried once priorities existed and produced 5086 underruns under a software
renderer. Something here still stops for longer than 43 milliseconds at a time.

`/etc/asound.conf` is the other half: the stock configuration for an HD Audio
card sends `default` through dmix, which mixes in the library using SysV
semaphores this kernel does not implement, and asks the driver for a software
volume control it does not have. `default` is the card with the plug layer above
it, so one program at a time gets the card. That is what a machine with no sound
server has anyway.

## Where the sound comes out

The card is always there; what was missing was anywhere for it to play. QEMU
was started with `-audiodev none`, which is a working driver and a silent
machine, and the two are hard to tell apart. The build now looks for a
PulseAudio socket, at `/mnt/wslg/PulseServer` under WSL or in
`XDG_RUNTIME_DIR` on a Linux desktop, and uses it where there is one.

## Why it arrived in pieces

Sound that plays but stutters is a different fault from sound that does not
play, and it was not in the guest. Measured from inside the driver, as the
longest the application went without feeding the card in each five second
window:

```
file sink (-audiodev wav):     41, 43, 48, 42, 44, 44, 41, 43 ms
PulseAudio, as it was:         41, 896, 111, 41, 50, 44, 262 ms
PulseAudio, with room:         41, 41, 44, 41, 41, 45, 48, 40 ms
```

The guest is identical in all three. What changed is the host: QEMU asks
PulseAudio for 15 milliseconds of latency by default, which is a fair bargain
when the sound server is a local process, and a bad one under WSL, where the
samples go over an RDP channel to Windows. When that channel takes its time it
stops the emulator with it, and 896 milliseconds is five times the whole
buffer, so what comes out is pieces. The build now asks for a tenth of a second
of latency and twice that of buffer.

A handful of glitches remained after that, in the first seconds while the game
loaded its assets, and they were not a buffering problem: raising the buffer
from 170 to 256 milliseconds removed two of eleven. Scheduling priorities
removed all of them.

## Verified

SuperTuxKart, run under weston, recorded through QEMU's `wav` backend:

```
recorded 86.7 seconds, peak amplitude 19747 of 32767, 62% of samples above 1000
underruns: 0
```

Recording is the only honest check available from here, since nobody is
listening: "it plays" has to mean "these samples left the machine".

`alsa-utils` is deliberately not on the image. It is the right tool for
checking sound by hand, and `aplay`, `speaker-test` and `amixer` all work, but
it ships a udev rule that runs `alsactl restore` when the control device
appears, and that hangs for the sixty seconds udev waits before giving up. Add
it once that is understood.
