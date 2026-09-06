/* Whether the playback pointer stays honest when the program is late. */
/* The hardware position wraps with the ring, so a pointer sampled only when
   userspace asks comes back wrong after a lap -- and a lap is a tenth of a
   second, less than one frame of a game that stalls the kernel. */

typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_ioctl 16
#define SYS_nanosleep 35
#define SYS_clock_gettime 228
#define SYS_fork 57
#define SYS_wait4 61
#define SYS_exit_group 231
#define CLOCK_MONOTONIC 1
#define O_RDWR 2
#define O_WRONLY_CREAT_TRUNC 0x241

static inline s64 syscall1(s64 n, s64 a) {
    s64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory");
    return r;
}
static inline s64 syscall2(s64 n, s64 a, s64 b) {
    s64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}
static inline s64 syscall3(s64 n, s64 a, s64 b, s64 c) {
    s64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

static inline s64 syscall4(s64 n, s64 a, s64 b, s64 c, s64 d) {
    s64 r;
    register s64 r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static int results_fd = -1;

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_write, 1, (s64)text, (s64)length);
    if (results_fd >= 0) (void)syscall3(SYS_write, results_fd, (s64)text, (s64)length);
}

static void put_signed(s64 value) {
    char buffer[24];
    int index = (int)sizeof(buffer);
    int negative = value < 0;
    u64 magnitude = negative ? (u64)(-value) : (u64)value;
    buffer[--index] = 0;
    do {
        buffer[--index] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (negative) buffer[--index] = '-';
    put(buffer + index);
}

static u64 now_ns(void) {
    struct { s64 seconds, nanoseconds; } value = {0, 0};
    (void)syscall2(SYS_clock_gettime, CLOCK_MONOTONIC, (s64)&value);
    return (u64)value.seconds * 1000000000UL + (u64)value.nanoseconds;
}

static void sleep_ns(u64 nanoseconds) {
    struct { s64 seconds, nanoseconds; } request;
    request.seconds = (s64)(nanoseconds / 1000000000UL);
    request.nanoseconds = (s64)(nanoseconds % 1000000000UL);
    (void)syscall2(SYS_nanosleep, (s64)&request, 0);
}

/* --- the slice of ALSA's ABI this needs ---------------------------------- */

#define MASK_WORDS 8
#define MASK_COUNT 3
#define INTERVAL_COUNT 12
#define PARAM_ACCESS 0
#define PARAM_FORMAT 1
#define PARAM_SUBFORMAT 2
#define PARAM_FIRST_INTERVAL 8
#define PARAM_SAMPLE_BITS 8
#define PARAM_FRAME_BITS 9
#define PARAM_CHANNELS 10
#define PARAM_RATE 11
#define PARAM_PERIOD_SIZE 13
#define PARAM_PERIODS 15
#define ACCESS_RW_INTERLEAVED 3
#define FORMAT_S16_LE 2
#define SUBFORMAT_STD 0

struct mask { u32 bits[MASK_WORDS]; };
struct interval {
    unsigned int min, max;
    unsigned int flags;          /* openmin:1 openmax:1 integer:1 empty:1 */
};
struct hw_params {
    unsigned int flags;
    struct mask masks[MASK_COUNT];
    struct mask reserved_masks[5];
    struct interval intervals[INTERVAL_COUNT];
    struct interval reserved_intervals[9];
    unsigned int rmask, cmask, info, msbits, rate_num, rate_den;
    unsigned long fifo_size;
    unsigned char reserved[64];
};
struct sw_params {
    int tstamp_mode;
    unsigned int period_step, sleep_min;
    unsigned long avail_min, xfer_align, start_threshold, stop_threshold;
    unsigned long silence_threshold, silence_size, boundary;
    unsigned int proto, tstamp_type;
    unsigned char reserved[56];
};
struct timespec_pair { s64 seconds, nanoseconds; };
struct pcm_status {
    int state, pad;
    struct timespec_pair trigger_tstamp, tstamp;
    unsigned long appl_ptr, hw_ptr;
    long delay;
    unsigned long avail, avail_max, overrange;
    int suspended_state;
    unsigned int audio_tstamp_data;
    struct timespec_pair audio_tstamp, driver_tstamp;
    unsigned int audio_tstamp_accuracy;
    unsigned char reserved[20];
};

#define IOC(dir, type, nr, size) \
    (((u64)(dir) << 30) | ((u64)(type) << 8) | ((u64)(nr)) | ((u64)(size) << 16))
#define IOWR(nr, type) IOC(3u, 'A', nr, sizeof(type))
#define IOW(nr, type)  IOC(1u, 'A', nr, sizeof(type))

#define NR_HW_PARAMS 0x11
#define NR_SW_PARAMS 0x13
#define NR_STATUS 0x20
#define NR_PREPARE 0x40
#define NR_START 0x42
#define NR_DROP 0x43
#define NR_WRITEI 0x50

struct writei { unsigned long result; const void *buffer; unsigned long frames; };

static int pcm = -1;

static void mask_set(struct mask *m, unsigned bit) { m->bits[bit / 32] |= 1U << (bit % 32); }
static void fix(struct hw_params *p, unsigned which, unsigned value) {
    struct interval *i = &p->intervals[which - PARAM_FIRST_INTERVAL];
    i->min = i->max = value;
    i->flags = 4;                 /* integer */
    p->rmask |= 1U << which;
}

#define RATE 48000U
#define CHANNELS 2U
#define PERIOD_FRAMES 1024U
#define PERIODS 4U
#define BUFFER_FRAMES (PERIOD_FRAMES * PERIODS)
#define FRAME_BYTES (CHANNELS * 2U)

static int configure(void) {
    static struct hw_params params;
    for (unsigned i = 0; i < sizeof(params); i++) ((char *)&params)[i] = 0;
    for (unsigned i = 0; i < INTERVAL_COUNT; i++) {
        params.intervals[i].min = 0;
        params.intervals[i].max = ~0U;
    }
    mask_set(&params.masks[PARAM_ACCESS], ACCESS_RW_INTERLEAVED);
    mask_set(&params.masks[PARAM_FORMAT], FORMAT_S16_LE);
    mask_set(&params.masks[PARAM_SUBFORMAT], SUBFORMAT_STD);
    params.rmask = ~0U;
    fix(&params, PARAM_CHANNELS, CHANNELS);
    fix(&params, PARAM_RATE, RATE);
    fix(&params, PARAM_PERIOD_SIZE, PERIOD_FRAMES);
    fix(&params, PARAM_PERIODS, PERIODS);
    fix(&params, PARAM_SAMPLE_BITS, 16);
    fix(&params, PARAM_FRAME_BITS, 16 * CHANNELS);
    if (syscall3(SYS_ioctl, pcm, (s64)IOWR(NR_HW_PARAMS, struct hw_params),
                 (s64)&params) != 0) return -1;

    static struct sw_params sw;
    for (unsigned i = 0; i < sizeof(sw); i++) ((char *)&sw)[i] = 0;
    sw.avail_min = PERIOD_FRAMES;
    sw.start_threshold = 1;
    /* Never stop on an underrun: the point of the test is to be late on
       purpose and still ask where the hardware got to. */
    sw.stop_threshold = ~0UL;
    if (syscall3(SYS_ioctl, pcm, (s64)IOWR(NR_SW_PARAMS, struct sw_params),
                 (s64)&sw) != 0) return -1;
    return 0;
}

static int status(struct pcm_status *out) {
    for (unsigned i = 0; i < sizeof(*out); i++) ((char *)out)[i] = 0;
    return (int)syscall3(SYS_ioctl, pcm, (s64)IOWR(NR_STATUS, struct pcm_status), (s64)out);
}

static short tone[BUFFER_FRAMES * CHANNELS];

/*
 * Write a whole buffer, start, then go away for longer than one lap and ask
 * where the hardware got to. The answer should be about as many frames as the
 * time that passed, whatever userspace did in between.
 */
static void test_pointer_survives_a_stall(u64 stall_ns) {
    if (syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_PREPARE, 0), 0) != 0) {
        put("SOUND prepare failed\n"); return;
    }
    struct writei write_request = { 0, tone, BUFFER_FRAMES };
    if (syscall3(SYS_ioctl, pcm, (s64)IOW(NR_WRITEI, struct writei),
                 (s64)&write_request) < 0) {
        put("SOUND writei failed\n"); return;
    }
    (void)syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_START, 0), 0);

    struct pcm_status before, after;
    if (status(&before) != 0) { put("SOUND status failed\n"); return; }
    u64 begun = now_ns();
    sleep_ns(stall_ns);
    u64 elapsed = now_ns() - begun;
    if (status(&after) != 0) { put("SOUND status failed\n"); return; }
    (void)syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_DROP, 0), 0);

    u64 advanced = after.hw_ptr - before.hw_ptr;
    u64 expected = elapsed / 1000000UL * RATE / 1000UL;
    put("SOUND stall_ms=");
    put_signed((s64)(elapsed / 1000000UL));
    put(" laps=");
    put_signed((s64)(expected * 10UL / BUFFER_FRAMES));   /* tenths */
    put(" hw_advanced=");
    put_signed((s64)advanced);
    put(" of_buffer=");
    put_signed(BUFFER_FRAMES);
    put(" state=");
    put_signed(after.state);
    put(after.state == 4 ? " (XRUN, noticed)" : " (RUNNING, not noticed)");
    put("\n");
}

/*
 * Playback that is fed properly, which must not be stopped by anything the
 * kernel does on its own. The tick samples the pointer between syscalls, and
 * a tick that also decided when the ring had run dry declared an underrun on
 * the ordinary gap between the hardware taking a frame and the writer being
 * scheduled -- which stopped the stream for good and was silence, not crackle.
 */
static int present_frames_until(u64 deadline_ns, unsigned *commits);
static void test_playback(u64 duration_ns, int presenting);

static void test_continuous_playback(u64 duration_ns) { return test_playback(duration_ns, 0); }

/* `presenting` forks a second process that pushes frames through the kernel for
   as long as the sound plays, which is the shape a game has. */
static void test_playback(u64 duration_ns, int presenting) {
    if (syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_PREPARE, 0), 0) != 0) {
        put("SOUND prepare failed\n"); return;
    }
    struct writei first = { 0, tone, BUFFER_FRAMES };
    if (syscall3(SYS_ioctl, pcm, (s64)IOW(NR_WRITEI, struct writei), (s64)&first) < 0) {
        put("SOUND writei failed\n"); return;
    }
    (void)syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_START, 0), 0);

    struct pcm_status begin_status;
    if (status(&begin_status) != 0) { put("SOUND status failed\n"); return; }

    u64 begun = now_ns();
    s64 painter = -1;
    if (presenting) {
        painter = syscall1(SYS_fork, 0);
        if (painter == 0) {
            unsigned commits = 0;
            (void)present_frames_until(begun + duration_ns, &commits);
            (void)syscall1(SYS_exit_group, 0);
        }
    }
    u64 written = 0;
    unsigned stalls = 0;
    unsigned empty = 0;         /* the ring ran dry: what a gap in the sound is */
    unsigned nearly = 0;        /* under a period left: the edge of one */
    struct pcm_status now_status = begin_status;
    while (now_ns() - begun < duration_ns) {
        if (status(&now_status) != 0) break;
        if (now_status.state != 3) { stalls++; break; }
        if (now_status.avail >= BUFFER_FRAMES) empty++;
        else if (now_status.avail > BUFFER_FRAMES - PERIOD_FRAMES) nearly++;
        u64 room = now_status.avail;
        if (room > PERIOD_FRAMES) room = PERIOD_FRAMES;
        if (room) {
            struct writei more = { 0, tone, room };
            if (syscall3(SYS_ioctl, pcm, (s64)IOW(NR_WRITEI, struct writei),
                         (s64)&more) < 0) { stalls++; break; }
            written += room;
        }
        sleep_ns(5000000UL);
    }
    u64 elapsed = now_ns() - begun;
    (void)status(&now_status);
    u64 advanced = now_status.hw_ptr - begin_status.hw_ptr;
    u64 expected = elapsed / 1000000UL * RATE / 1000UL;
    (void)syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_DROP, 0), 0);

    put("SOUND played_ms=");
    put_signed((s64)(elapsed / 1000000UL));
    put(" frames_written=");
    put_signed((s64)written);
    put(" hw_advanced=");
    put_signed((s64)advanced);
    put(" expected=");
    put_signed((s64)expected);
    if (painter > 0) (void)syscall4(SYS_wait4, painter, 0, 0, 0);
    put(presenting ? " presenting=yes" : " presenting=no");
    put(" ran_dry=");
    put_signed((s64)empty);
    put(" nearly_dry=");
    put_signed((s64)nearly);
    put(" state=");
    put_signed(now_status.state);
    put(now_status.state == 3 ? " (RUNNING)" : " (STOPPED)");
    put(stalls ? " STALLED\n" : "\n");
}

/*
 * An underrun, and then the recovery every ALSA program makes: prepare, write,
 * start. If that does not put the stream back, the first stall a program hits
 * is the last sound it makes -- silence rather than a gap.
 */
static void test_xrun_recovery(void) {
    if (syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_PREPARE, 0), 0) != 0) {
        put("SOUND prepare failed\n"); return;
    }
    struct writei first = { 0, tone, BUFFER_FRAMES };
    (void)syscall3(SYS_ioctl, pcm, (s64)IOW(NR_WRITEI, struct writei), (s64)&first);
    (void)syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_START, 0), 0);

    /* Long enough that the ring has certainly run dry. */
    sleep_ns(300000000UL);
    struct pcm_status drained;
    (void)status(&drained);

    /* The recovery, exactly as alsa-lib does it. */
    s64 prepared = syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_PREPARE, 0), 0);
    struct writei again = { 0, tone, BUFFER_FRAMES };
    s64 rewritten = syscall3(SYS_ioctl, pcm, (s64)IOW(NR_WRITEI, struct writei), (s64)&again);
    s64 restarted = syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_START, 0), 0);

    struct pcm_status before_run, after_run;
    (void)status(&before_run);
    sleep_ns(50000000UL);
    (void)status(&after_run);
    (void)syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_DROP, 0), 0);

    u64 advanced = after_run.hw_ptr - before_run.hw_ptr;
    put("SOUND after_drain state=");
    put_signed(drained.state);
    put(" prepare=");
    put_signed(prepared);
    put(" write=");
    put_signed(rewritten);
    put(" start=");
    put_signed(restarted);
    put(" then_state=");
    put_signed(after_run.state);
    put(" advanced=");
    put_signed((s64)advanced);
    put(advanced > 1000 ? " PLAYING\n" : " SILENT\n");
}


/* --- the frame the sound has to survive ---------------------------------- */
/*
 * A second process presenting frames as fast as it can, which is what a game
 * does while it plays sound. Every present is a whole screen through the
 * kernel, and audio that runs dry underneath it is what crackles.
 */
#define DRM_TYPE 'd'
#define NR_MODE_GETCRTC 0xa1
#define NR_MODE_CREATE_DUMB 0xb2
#define NR_MODE_ADDFB2 0xb8
#define NR_MODE_ATOMIC 0xbc
#define NR_MODE_CREATEPROPBLOB 0xbd

struct drm_mode_crtc {
    u64 set_connectors_ptr; u32 count_connectors;
    u32 crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct drm_mode_modeinfo_snd { char bytes[68]; } mode;
};

struct drm_mode_create_dumb {
    u32 height, width, bpp, flags; u32 handle, pitch; u64 size;
};

struct drm_mode_fb_cmd2 {
    u32 fb_id, width, height, pixel_format, flags;
    u32 handles[4]; u32 pitches[4]; u32 offsets[4]; u64 modifier[4];
};

struct drm_mode_create_blob { u64 data; u32 length; u32 blob_id; };

struct drm_mode_atomic {
    u32 flags; u32 count_objs;
    u64 objs_ptr;
    u64 count_props_ptr;
    u64 props_ptr;
    u64 prop_values_ptr;
    u64 reserved;
    u64 user_data;
};

#define DRM_IOWR(nr, type) IOC(3u, DRM_TYPE, nr, sizeof(type))

static int present_frames_until(u64 deadline_ns, unsigned *commits) {
    int card = (int)syscall3(SYS_open, (s64)"/dev/dri/card0", O_RDWR, 0);
    if (card < 0) return -1;

    struct drm_mode_crtc crtc;
    for (unsigned i = 0; i < sizeof(crtc); i++) ((char *)&crtc)[i] = 0;
    crtc.crtc_id = 1;
    if (syscall3(SYS_ioctl, card, (s64)DRM_IOWR(NR_MODE_GETCRTC, struct drm_mode_crtc),
                 (s64)&crtc) != 0) return -1;
    unsigned short width = *(unsigned short *)(crtc.mode.bytes + 4);
    unsigned short height = *(unsigned short *)(crtc.mode.bytes + 14);

    struct drm_mode_create_dumb create;
    for (unsigned i = 0; i < sizeof(create); i++) ((char *)&create)[i] = 0;
    create.width = width; create.height = height; create.bpp = 32;
    if (syscall3(SYS_ioctl, card, (s64)DRM_IOWR(NR_MODE_CREATE_DUMB, struct drm_mode_create_dumb),
                 (s64)&create) != 0) return -1;

    struct drm_mode_fb_cmd2 fb;
    for (unsigned i = 0; i < sizeof(fb); i++) ((char *)&fb)[i] = 0;
    fb.width = width; fb.height = height; fb.pixel_format = 0x34325258;
    fb.handles[0] = create.handle; fb.pitches[0] = create.pitch;
    if (syscall3(SYS_ioctl, card, (s64)DRM_IOWR(NR_MODE_ADDFB2, struct drm_mode_fb_cmd2),
                 (s64)&fb) != 0) return -1;

    struct drm_mode_create_blob blob;
    for (unsigned i = 0; i < sizeof(blob); i++) ((char *)&blob)[i] = 0;
    blob.data = (u64)crtc.mode.bytes; blob.length = 68;
    if (syscall3(SYS_ioctl, card, (s64)DRM_IOWR(NR_MODE_CREATEPROPBLOB, struct drm_mode_create_blob),
                 (s64)&blob) != 0) return -1;

    u32 objs[3]   = { 1, 2, 4 };
    u32 counts[3] = { 2, 1, 10 };
    u32 props[13] = { 11, 12, 15, 14, 13, 16, 17, 18, 19, 20, 21, 22, 23 };
    u64 values[13] = { 1, blob.blob_id, 1, fb.fb_id, 1,
                       0, 0, (u64)width << 16, (u64)height << 16, 0, 0, width, height };
    struct drm_mode_atomic atomic;
    for (unsigned i = 0; i < sizeof(atomic); i++) ((char *)&atomic)[i] = 0;
    atomic.count_objs = 3;
    atomic.objs_ptr = (u64)objs;
    atomic.count_props_ptr = (u64)counts;
    atomic.props_ptr = (u64)props;
    atomic.prop_values_ptr = (u64)values;

    while (now_ns() < deadline_ns) {
        if (syscall3(SYS_ioctl, card, (s64)DRM_IOWR(NR_MODE_ATOMIC, struct drm_mode_atomic),
                     (s64)&atomic) != 0) break;
        (*commits)++;
    }
    return 0;
}

/* The tick's own string instruction, met with the direction flag set: a starved
   ring is silenced from the tick, and that silencing is a memset. */
#define DF_GUARD 4096U
#define DF_PATTERN 0x5A
static unsigned char df_area[DF_GUARD * 2U];

static int df_spin_returns_flag(u64 spins) {
    u64 flags = 0;
    __asm__ volatile("std\n\t"
                     "1: dec %[n]\n\t"
                     "jnz 1b\n\t"
                     "pushfq\n\t"
                     "pop %[out]\n\t"
                     "cld"
                     : [out] "=&r"(flags), [n] "+r"(spins)
                     : : "cc", "memory");
    return (int)((flags >> 10) & 1U);
}

/* Called while the ring is starved, so the tick is silencing it every 4 ms. */
static void test_direction_flag_under_silencing(unsigned rounds) {
    unsigned damaged = 0, lost = 0;
    for (unsigned round = 0; round < rounds; round++) {
        for (unsigned i = 0; i < sizeof(df_area); i++) df_area[i] = DF_PATTERN;
        if (!df_spin_returns_flag(4000000UL)) lost++;
        for (unsigned i = 0; i < sizeof(df_area); i++)
            if (df_area[i] != DF_PATTERN) { damaged++; break; }
    }
    put("DF rounds=");
    put_signed((s64)rounds);
    put(" guard_damaged=");
    put_signed((s64)damaged);
    put(" flag_lost=");
    put_signed((s64)lost);
    put(damaged || lost ? " BROKEN\n" : " CLEAN\n");
}

static int run(void) {
    results_fd = (int)syscall3(SYS_open, (s64)"/tunix-soundtest-results.txt",
                               O_WRONLY_CREAT_TRUNC, 0644);
    pcm = (int)syscall3(SYS_open, (s64)"/dev/snd/pcmC0D0p", O_RDWR, 0);
    if (pcm < 0) {
        put("SOUND no pcm device\n");
        put("SOUNDTEST DONE\n");
        return 0;
    }
    for (unsigned i = 0; i < BUFFER_FRAMES * CHANNELS; i++)
        tone[i] = (short)((i % 128) * 256 - 16384);
    if (configure() != 0) {
        put("SOUND configure failed\n");
        put("SOUNDTEST DONE\n");
        return 0;
    }
    put("SOUND configured rate=");
    put_signed(RATE);
    put(" buffer_frames=");
    put_signed(BUFFER_FRAMES);
    put(" lap_ms=");
    put_signed(BUFFER_FRAMES * 1000UL / RATE);
    put("\n");

    /* Inside a lap, which always worked, and then past one, which did not. */
    test_continuous_playback(5000000000UL);
    test_playback(5000000000UL, 1);
    test_xrun_recovery();
    test_pointer_survives_a_stall(40000000UL);
    test_pointer_survives_a_stall(200000000UL);
    test_pointer_survives_a_stall(500000000UL);

    /* Starved on purpose so the tick is silencing the ring, and the flag held
       set across it. */
    if (syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_PREPARE, 0), 0) == 0) {
        struct writei burst = { 0, tone, BUFFER_FRAMES };
        (void)syscall3(SYS_ioctl, pcm, (s64)IOW(NR_WRITEI, struct writei), (s64)&burst);
        (void)syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_START, 0), 0);
        sleep_ns(200000000UL);
        test_direction_flag_under_silencing(60);
        (void)syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_DROP, 0), 0);
    }

    /* Ends starved on purpose and stays that way: whatever the card plays from
       here is what an underrun sounds like, and the tail of the recording is
       checked for it. A ring nobody refills is replayed by the engine for ever
       unless the driver silences what is in front of the writer. */
    if (syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_PREPARE, 0), 0) == 0) {
        struct writei burst = { 0, tone, BUFFER_FRAMES };
        (void)syscall3(SYS_ioctl, pcm, (s64)IOW(NR_WRITEI, struct writei), (s64)&burst);
        (void)syscall3(SYS_ioctl, pcm, (s64)IOC(0u, 'A', NR_START, 0), 0);
        sleep_ns(1500000000UL);
    }
    put("SOUNDTEST DONE\n");
    return 0;
}

static void run_and_park(void) __attribute__((noreturn, used));
static void run_and_park(void) {
    (void)run();
    for (;;) sleep_ns(1000000000UL);
}

__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    and $-16, %rsp\n"
        "    call run_and_park\n"
        "    hlt\n");
