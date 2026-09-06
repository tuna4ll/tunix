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
static void test_continuous_playback(u64 duration_ns) {
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
    u64 written = 0;
    unsigned stalls = 0;
    struct pcm_status now_status = begin_status;
    while (now_ns() - begun < duration_ns) {
        if (status(&now_status) != 0) break;
        if (now_status.state != 3) { stalls++; break; }
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
    put(" state=");
    put_signed(now_status.state);
    put(now_status.state == 3 ? " (RUNNING)" : " (STOPPED)");
    put(stalls ? " STALLED\n" : "\n");
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
    test_continuous_playback(1000000000UL);
    test_pointer_survives_a_stall(40000000UL);
    test_pointer_survives_a_stall(200000000UL);
    test_pointer_survives_a_stall(500000000UL);
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
