#define TEST_NAME "sound"
#include "test.h"

#if defined(__x86_64__)
#define SYS_finit_module 313
#define SYS_ppoll        271
#else
#define SYS_finit_module 273
#define SYS_ppoll        73
#endif

#define EBUSY   16
#define EBADFD  77
#define POLLIN  0x001
#define POLLOUT 0x004

#define RATE    48000U
#define PERIOD  1024U
#define PERIODS 4U
#define BUFFER  (PERIOD * PERIODS)

#define IOC(dir, type, nr, size) (((u64)(dir) << 30) | ((u64)(size) << 16) | ((type) << 8) | (nr))
#define IOR(type, nr, size)      IOC(2U, type, nr, size)
#define IOW(type, nr, size)      IOC(1U, type, nr, size)
#define IOWR(type, nr, size)     IOC(3U, type, nr, size)

struct snd_interval {
    u32 min;
    u32 max;
    u32 flags;
};

#define INTERVAL_OPENMIN 1U
#define INTERVAL_OPENMAX 2U
#define INTERVAL_INTEGER 4U
#define INTERVAL_EMPTY   8U

struct hw_params {
    u32 flags;
    u32 masks[3][8];
    u32 reserved_masks[5][8];
    struct snd_interval intervals[12];
    struct snd_interval reserved_intervals[9];
    u32 rmask;
    u32 cmask;
    u32 info;
    u32 msbits;
    u32 rate_num;
    u32 rate_den;
    u64 fifo_size;
    u8 reserved[64];
};

struct sw_params {
    int tstamp_mode;
    u32 period_step;
    u32 sleep_min;
    u64 avail_min;
    u64 xfer_align;
    u64 start_threshold;
    u64 stop_threshold;
    u64 silence_threshold;
    u64 silence_size;
    u64 boundary;
    u32 proto;
    u32 tstamp_type;
    u8 reserved[56];
};

struct timespec64 {
    s64 sec;
    s64 nsec;
};

struct pcm_status {
    int state;
    int pad;
    struct timespec64 trigger_tstamp;
    struct timespec64 tstamp;
    u64 appl_ptr;
    u64 hw_ptr;
    s64 delay;
    u64 avail;
    u64 avail_max;
    u64 overrange;
    int suspended_state;
    u32 audio_tstamp_data;
    struct timespec64 audio_tstamp;
    struct timespec64 driver_tstamp;
    u32 audio_tstamp_accuracy;
    u8 reserved[20];
};

struct xferi {
    s64 result;
    u64 buf;
    u64 frames;
};

struct sync_ptr {
    u32 flags;
    u32 pad;
    union {
        struct {
            int state;
            int pad1;
            u64 hw_ptr;
            struct timespec64 tstamp;
            int suspended_state;
            int pad2;
            struct timespec64 audio_tstamp;
        } status;
        u8 reserved[64];
    } s;
    union {
        struct {
            u64 appl_ptr;
            u64 avail_min;
        } control;
        u8 reserved[64];
    } c;
};

struct card_info {
    int card;
    int pad;
    u8 id[16];
    u8 driver[16];
    u8 name[32];
    u8 longname[80];
    u8 reserved[16];
    u8 mixername[80];
    u8 components[128];
};

struct elem_id {
    u32 numid;
    int iface;
    u32 device;
    u32 subdevice;
    u8 name[44];
    u32 index;
};

struct elem_list {
    u32 offset;
    u32 space;
    u32 used;
    u32 count;
    u64 pids;
    u8 reserved[50];
};

struct elem_info {
    struct elem_id id;
    int type;
    u32 access;
    u32 count;
    int owner;
    union {
        struct {
            s64 min;
            s64 max;
            s64 step;
        } integer;
        u8 reserved[128];
    } value;
    u16 dimen[4];
    u8 reserved[56];
};

struct elem_value {
    struct elem_id id;
    u32 indirect;
    union {
        s64 integer[128];
        u8 reserved[1024];
    } value;
    struct timespec64 tstamp;
    u8 reserved[112];
};

struct ctl_event {
    int type;
    union {
        struct {
            u32 mask;
            struct elem_id id;
        } elem;
        u8 data8[60];
    } data;
};

struct ctl_tlv {
    u32 numid;
    u32 length;
    u32 tlv[4];
};

struct pollfd {
    int fd;
    short events;
    short revents;
};

_Static_assert(sizeof(struct hw_params) == 608, "hw_params");
_Static_assert(sizeof(struct sw_params) == 136, "sw_params");
_Static_assert(sizeof(struct pcm_status) == 152, "status");
_Static_assert(sizeof(struct sync_ptr) == 136, "sync_ptr");
_Static_assert(sizeof(struct card_info) == 376, "card_info");
_Static_assert(sizeof(struct elem_list) == 80, "elem_list");
_Static_assert(sizeof(struct elem_info) == 272, "elem_info");
_Static_assert(sizeof(struct elem_value) == 1224, "elem_value");
_Static_assert(sizeof(struct ctl_event) == 72, "ctl_event");

#define PCM_HW_REFINE   IOWR('A', 0x10, sizeof(struct hw_params))
#define PCM_HW_PARAMS   IOWR('A', 0x11, sizeof(struct hw_params))
#define PCM_SW_PARAMS   IOWR('A', 0x13, sizeof(struct sw_params))
#define PCM_STATUS      IOR('A', 0x20, sizeof(struct pcm_status))
#define PCM_SYNC_PTR    IOWR('A', 0x23, sizeof(struct sync_ptr))
#define PCM_PREPARE     IOC(0U, 'A', 0x40, 0)
#define PCM_START       IOC(0U, 'A', 0x42, 0)
#define PCM_DROP        IOC(0U, 'A', 0x43, 0)
#define PCM_DRAIN       IOC(0U, 'A', 0x44, 0)
#define PCM_PAUSE       IOW('A', 0x45, sizeof(int))
#define PCM_REWIND      IOW('A', 0x46, sizeof(u64))
#define PCM_FORWARD     IOW('A', 0x49, sizeof(u64))
#define PCM_WRITEI      IOW('A', 0x50, sizeof(struct xferi))
#define PCM_READI       IOR('A', 0x51, sizeof(struct xferi))
#define CTL_CARD_INFO   IOR('U', 0x01, sizeof(struct card_info))
#define CTL_ELEM_LIST   IOWR('U', 0x10, sizeof(struct elem_list))
#define CTL_ELEM_INFO   IOWR('U', 0x11, sizeof(struct elem_info))
#define CTL_ELEM_READ   IOWR('U', 0x12, sizeof(struct elem_value))
#define CTL_ELEM_WRITE  IOWR('U', 0x13, sizeof(struct elem_value))
#define CTL_SUBSCRIBE   IOWR('U', 0x16, sizeof(int))
#define CTL_TLV_READ    IOWR('U', 0x1a, 8)
#define CTL_TLV_WRITE   IOWR('U', 0x1b, 8)
#define CTL_ELEM_ADD    IOWR('U', 0x17, sizeof(struct elem_info))
#define CTL_ELEM_REMOVE IOWR('U', 0x19, sizeof(struct elem_id))

struct timer_id {
    int dev_class;
    int dev_sclass;
    int card;
    int device;
    int subdevice;
};

struct timer_select {
    struct timer_id id;
    u8 reserved[32];
};

struct timer_params {
    u32 flags;
    u32 ticks;
    u32 queue_size;
    u32 reserved0;
    u32 filter;
    u8 reserved[60];
};

struct timer_tread {
    int event;
    int pad1;
    s64 sec;
    s64 nsec;
    u32 value;
    int pad2;
};

#define TIMER_PVERSION IOR('T', 0x00, sizeof(int))
#define TIMER_TREAD    IOW('T', 0x02, sizeof(int))
#define TIMER_SELECT   IOW('T', 0x10, sizeof(struct timer_select))
#define TIMER_PARAMS   IOW('T', 0x12, sizeof(struct timer_params))
#define TIMER_START    IOC(0U, 'T', 0xa0, 0)
#define TIMER_STOP     IOC(0U, 'T', 0xa1, 0)

#define STATE_SETUP    1
#define STATE_PREPARED 2
#define STATE_RUNNING  3
#define STATE_XRUN     4
#define STATE_PAUSED   6

#define PARAM_ACCESS      0
#define PARAM_FORMAT      1
#define PARAM_SUBFORMAT   2
#define PARAM_SAMPLE_BITS 8
#define PARAM_CHANNELS    10
#define PARAM_RATE        11
#define PARAM_PERIOD_TIME 12
#define PARAM_PERIOD_SIZE 13
#define PARAM_PERIODS     15
#define PARAM_BUFFER_TIME 16
#define PARAM_BUFFER_SIZE 17

#define ACCESS_MMAP 0
#define ACCESS_RW   3
#define FORMAT_S16  2

static s64 ppoll1(int fd, short events, int timeout_ms) {
    struct pollfd entry = {fd, events, 0};
    struct timespec limit = {timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
    s64 result = SYSCALL(SYS_ppoll, &entry, 1, &limit, 0, 8, 0);
    if (result <= 0) return result;
    return entry.revents;
}

static s64 now_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.sec * 1000000L + now.nsec / 1000L;
}

static struct snd_interval *interval(struct hw_params *params, unsigned var) {
    return &params->intervals[var - 8U];
}

static void params_any(struct hw_params *params) {
    u8 *bytes = (u8 *)params;
    for (u64 at = 0; at < sizeof(*params); at++) bytes[at] = 0;
    for (unsigned mask = 0; mask < 3; mask++)
        for (unsigned word = 0; word < 8; word++) params->masks[mask][word] = ~0U;
    for (unsigned index = 0; index < 12; index++) {
        params->intervals[index].min = 0;
        params->intervals[index].max = ~0U;
    }
    params->rmask = ~0U;
}

static void set_mask(struct hw_params *params, unsigned var, unsigned bit) {
    for (unsigned word = 0; word < 8; word++) params->masks[var][word] = 0;
    params->masks[var][bit / 32U] = 1U << (bit % 32U);
}

static void set_value(struct hw_params *params, unsigned var, u32 value) {
    interval(params, var)->min = value;
    interval(params, var)->max = value;
    interval(params, var)->flags = INTERVAL_INTEGER;
}

static int single(struct hw_params *params, unsigned var, u32 value) {
    struct snd_interval *it = interval(params, var);
    return it->min == value && it->max == value && !(it->flags & INTERVAL_EMPTY);
}

static int configure(int fd, unsigned access, u32 period, u32 periods) {
    struct hw_params params;
    params_any(&params);
    set_mask(&params, PARAM_ACCESS, access);
    set_mask(&params, PARAM_FORMAT, FORMAT_S16);
    set_value(&params, PARAM_CHANNELS, 2);
    set_value(&params, PARAM_RATE, RATE);
    set_value(&params, PARAM_PERIOD_SIZE, period);
    set_value(&params, PARAM_PERIODS, periods);
    if (ioctl(fd, PCM_HW_PARAMS, &params) != 0) return -1;
    if (!single(&params, PARAM_BUFFER_SIZE, period * periods)) return -2;
    return 0;
}

static int set_thresholds(int fd, u64 start, u64 stop) {
    struct sw_params params;
    u8 *bytes = (u8 *)&params;
    for (u64 at = 0; at < sizeof(params); at++) bytes[at] = 0;
    params.avail_min = PERIOD;
    params.start_threshold = start;
    params.stop_threshold = stop;
    params.proto = (2U << 16) | (0U << 8) | 14U;
    params.tstamp_type = 1;
    return (int)ioctl(fd, PCM_SW_PARAMS, &params);
}

static int pcm_state(int fd, struct pcm_status *status) {
    if (ioctl(fd, PCM_STATUS, status) != 0) return -1;
    return status->state;
}

static u16 tone[2 * RATE * 2];

static void make_pattern(void) {
    for (u32 frame = 0; frame < 2U * RATE; frame++) {
        u32 phase = frame % 48U;
        s64 value = phase < 24U ? (s64)phase * 1000 - 12000 : (s64)(47U - phase) * 1000 - 11000;
        tone[frame * 2U] = (u16)(short)value;
        tone[frame * 2U + 1U] = (u16)(0x4000U | (frame & 0x3FFFU));
    }
}

static s64 writei(int fd, const void *buffer, u64 frames) {
    struct xferi transfer = {0, (u64)buffer, frames};
    s64 status = ioctl(fd, PCM_WRITEI, &transfer);
    return status < 0 ? status : transfer.result;
}

static s64 readi(int fd, void *buffer, u64 frames) {
    struct xferi transfer = {0, (u64)buffer, frames};
    s64 status = ioctl(fd, PCM_READI, &transfer);
    return status < 0 ? status : transfer.result;
}

static int find_element(int fd, const char *name, struct elem_id *found) {
    struct elem_id ids[64];
    struct elem_list list = {0, 64, 0, 0, (u64)ids, {0}};
    if (ioctl(fd, CTL_ELEM_LIST, &list) != 0) return -1;
    for (u32 index = 0; index < list.used; index++)
        if (streq((const char *)ids[index].name, name)) {
            *found = ids[index];
            return 0;
        }
    return -1;
}

static int read_text(const char *path, char *text, u64 room) {
    s64 fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    s64 length = read((int)fd, text, room - 1);
    close((int)fd);
    if (length < 0) return -1;
    text[length] = 0;
    return 0;
}

static int contains(const char *text, const char *needle) {
    u64 wanted = strlen(needle);
    for (u64 at = 0; text[at]; at++)
        if (memeq(text + at, needle, wanted)) return 1;
    return 0;
}

static void controls(void) {
    s64 fd = open("/dev/snd/controlC0", O_RDWR, 0);
    expect(fd >= 0, "controlC0 opens");
    if (fd < 0) return;
    struct card_info card;
    expect_eq(ioctl((int)fd, CTL_CARD_INFO, &card), 0, "CARD_INFO answers");
    expect(streq((const char *)card.driver, "HDA-Intel"), "the driver is HDA-Intel");
    expect(contains((const char *)card.components, "HDA:"), "components name the codec");

    struct elem_id volume;
    expect_eq(find_element((int)fd, "Master Playback Volume", &volume), 0,
              "the mixer has Master Playback Volume");
    struct elem_id jack;
    expect_eq(find_element((int)fd, "Line Out Phantom Jack", &jack), 0,
              "the fixed line out has a phantom jack control");
    struct elem_info info;
    u8 *bytes = (u8 *)&info;
    for (u64 at = 0; at < sizeof(info); at++) bytes[at] = 0;
    info.id = volume;
    expect_eq(ioctl((int)fd, CTL_ELEM_INFO, &info), 0, "ELEM_INFO answers");
    expect(info.type == 2 && info.count == 2 && info.value.integer.max > 0,
           "the volume is a stereo integer with a range");
    struct ctl_tlv tlv = {volume.numid, sizeof(tlv.tlv), {0}};
    expect_eq(ioctl((int)fd, CTL_TLV_READ, &tlv), 0, "TLV_READ answers");
    expect(tlv.tlv[0] == 1 && tlv.tlv[1] == 8 && (tlv.tlv[3] & 0xFFFFU) > 0,
           "the volume carries a dB scale");

    int on = 1;
    expect_eq(ioctl((int)fd, CTL_SUBSCRIBE, &on), 0, "events can be subscribed");
    struct elem_value value;
    bytes = (u8 *)&value;
    for (u64 at = 0; at < sizeof(value); at++) bytes[at] = 0;
    value.id = volume;
    expect_eq(ioctl((int)fd, CTL_ELEM_READ, &value), 0, "the volume reads");
    s64 old = value.value.integer[0];
    s64 wanted = old > 0 ? old - 1 : old + 1;
    value.value.integer[0] = wanted;
    value.value.integer[1] = wanted;
    expect_eq(ioctl((int)fd, CTL_ELEM_WRITE, &value), 0, "the volume writes");
    value.value.integer[0] = -1;
    expect_eq(ioctl((int)fd, CTL_ELEM_READ, &value), 0, "and reads again");
    expect_eq(value.value.integer[0], wanted, "with the new value");
    expect((ppoll1((int)fd, POLLIN, 1000) & POLLIN) != 0, "the change wakes a poller");
    struct ctl_event event;
    expect_eq(read((int)fd, &event, sizeof(event)), (s64)sizeof(event), "an event is read");
    expect(event.type == 0 && (event.data.elem.mask & 1U) &&
               event.data.elem.id.numid == volume.numid,
           "it reports the volume value change");
    expect_eq(ioctl((int)fd, CTL_ELEM_WRITE, &value), 0, "writing the same value works");
    expect_eq(ppoll1((int)fd, POLLIN, 100), 0, "but raises no event");
    value.value.integer[0] = old;
    value.value.integer[1] = old;
    expect_eq(ioctl((int)fd, CTL_ELEM_WRITE, &value), 0, "the volume is restored");
    while (ppoll1((int)fd, POLLIN, 50) > 0) read((int)fd, &event, sizeof(event));

    for (u64 at = 0; at < sizeof(info); at++) ((u8 *)&info)[at] = 0;
    info.id.iface = 2;
    const char *label = "Test Playback Volume";
    for (u64 at = 0; label[at]; at++) info.id.name[at] = (u8)label[at];
    info.type = 2;
    info.count = 2;
    info.access = 3;
    info.value.integer.min = 0;
    info.value.integer.max = 255;
    info.value.integer.step = 1;
    expect_eq(ioctl((int)fd, CTL_ELEM_ADD, &info), 0, "a user control is added");
    u32 user_numid = info.id.numid;
    expect(user_numid > volume.numid, "and gets its own number");
    expect((ppoll1((int)fd, POLLIN, 500) & POLLIN) != 0, "the addition raises an event");
    expect_eq(read((int)fd, &event, sizeof(event)), (s64)sizeof(event), "which is read");
    expect((event.data.elem.mask & 4U) && event.data.elem.id.numid == user_numid,
           "and announces the new element");
    expect_eq(ioctl((int)fd, CTL_ELEM_ADD, &info), -EBUSY, "the same name cannot be added twice");
    struct elem_id found;
    expect_eq(find_element((int)fd, label, &found), 0, "the list includes it");
    for (u64 at = 0; at < sizeof(value); at++) ((u8 *)&value)[at] = 0;
    value.id = found;
    value.value.integer[0] = 200;
    value.value.integer[1] = 100;
    expect_eq(ioctl((int)fd, CTL_ELEM_WRITE, &value), 0, "it stores a value");
    value.value.integer[0] = 0;
    expect_eq(ioctl((int)fd, CTL_ELEM_READ, &value), 0, "it reads back");
    expect(value.value.integer[0] == 200 && value.value.integer[1] == 100, "per channel");
    value.value.integer[0] = 300;
    expect_eq(ioctl((int)fd, CTL_ELEM_WRITE, &value), -22, "values out of range are refused");
    struct ctl_tlv user_tlv = {user_numid, 16, {1, 8, (u32)-5100, 20}};
    expect_eq(ioctl((int)fd, CTL_TLV_WRITE, &user_tlv), 0, "a dB scale is attached");
    struct ctl_tlv back = {user_numid, sizeof(back.tlv), {0}};
    expect_eq(ioctl((int)fd, CTL_TLV_READ, &back), 0, "and read back");
    expect(back.tlv[0] == 1 && back.tlv[2] == (u32)-5100 && back.tlv[3] == 20, "unchanged");
    while (ppoll1((int)fd, POLLIN, 50) > 0) read((int)fd, &event, sizeof(event));
    expect_eq(ioctl((int)fd, CTL_ELEM_REMOVE, &found), 0, "the user control is removed");
    expect((ppoll1((int)fd, POLLIN, 500) & POLLIN) != 0, "removal raises an event");
    read((int)fd, &event, sizeof(event));
    expect(event.data.elem.mask == ~0U, "marked as removal");
    info.id.numid = user_numid;
    expect_eq(ioctl((int)fd, CTL_ELEM_INFO, &info), -ENOENT, "the element is gone");
    expect_eq(ioctl((int)fd, CTL_ELEM_REMOVE, &volume), -1, "driver controls cannot be removed");
    close((int)fd);
}

static void playback(void) {
    s64 fd = open("/dev/snd/pcmC0D0p", O_RDWR, 0);
    expect(fd >= 0, "the playback device opens");
    if (fd < 0) return;
    expect_eq(open("/dev/snd/pcmC0D0p", O_RDWR, 0), -EBUSY, "a second open is refused");

    struct hw_params params;
    params_any(&params);
    expect_eq(ioctl((int)fd, PCM_HW_REFINE, &params), 0, "HW_REFINE accepts anything");
    expect(interval(&params, PARAM_RATE)->min <= RATE && interval(&params, PARAM_RATE)->max >= RATE,
           "48 kHz is offered");
    expect((params.masks[PARAM_FORMAT][0] & (1U << FORMAT_S16)) != 0, "S16_LE is offered");

    params_any(&params);
    set_mask(&params, PARAM_FORMAT, FORMAT_S16);
    set_value(&params, PARAM_CHANNELS, 2);
    set_value(&params, PARAM_RATE, RATE);
    set_value(&params, PARAM_PERIOD_TIME, 21333);
    interval(&params, PARAM_PERIOD_TIME)->flags = 0;
    interval(&params, PARAM_PERIOD_TIME)->max = 21334;
    expect_eq(ioctl((int)fd, PCM_HW_REFINE, &params), 0, "a period time near 1024 frames refines");
    expect(interval(&params, PARAM_PERIOD_SIZE)->min <= 1024 &&
               interval(&params, PARAM_PERIOD_SIZE)->max >= 1024,
           "and keeps 1024 frames possible");
    set_value(&params, PARAM_PERIOD_SIZE, 1024);
    set_value(&params, PARAM_BUFFER_TIME, 170666);
    interval(&params, PARAM_BUFFER_TIME)->flags = 0;
    interval(&params, PARAM_BUFFER_TIME)->max = 170667;
    expect_eq(ioctl((int)fd, PCM_HW_REFINE, &params), 0, "a buffer time of eight periods refines");
    expect(single(&params, PARAM_BUFFER_SIZE, 8192) && single(&params, PARAM_PERIODS, 8),
           "to exactly 8 x 1024 frames");

    expect_eq(configure((int)fd, ACCESS_RW, PERIOD, PERIODS), 0, "4 x 1024 frames at 48 kHz");
    expect_eq(set_thresholds((int)fd, BUFFER, BUFFER), 0, "thresholds are set");
    expect_eq(ioctl((int)fd, PCM_PREPARE, 0), 0, "the stream prepares");

    s64 start = now_us();
    s64 written = writei((int)fd, tone, 2U * RATE);
    s64 spent = now_us() - start;
    expect_eq(written, (s64)(2U * RATE), "a blocking write of two seconds is taken whole");
    expect(spent > 1850000 && spent < 2150000, "at the hardware rate");
    if (spent <= 1850000 || spent >= 2150000) {
        print("sound: two seconds took ");
        print_number(spent / 1000);
        print(" ms\n");
    }
    struct pcm_status status;
    expect_eq(pcm_state((int)fd, &status), STATE_RUNNING, "the stream runs");
    expect(status.avail <= BUFFER && status.delay >= 0, "the status is consistent");

    char text[1024];
    expect(read_text("/proc/asound/card0/pcm0p/sub0/status", text, sizeof(text)) == 0 &&
               contains(text, "RUNNING"),
           "/proc/asound shows the stream running");
    expect(read_text("/proc/asound/cards", text, sizeof(text)) == 0 && contains(text, "HDA-Intel"),
           "/proc/asound/cards lists the card");
    char attribute[64];
    expect(read_text("/sys/class/sound/card0/number", attribute, sizeof(attribute)) == 0 &&
               streq(attribute, "0\n"),
           "sysfs has the card with its number");
    expect(read_text("/sys/class/sound/controlC0/device/id", attribute, sizeof(attribute)) == 0 &&
               contains(attribute, "HDAIntel"),
           "and the control device hangs below it");
    expect(read_text("/sys/class/sound/card0/uevent", attribute, sizeof(attribute)) == 0 &&
               contains(attribute, "SUBSYSTEM=sound") && !contains(attribute, "DEVNAME"),
           "the card itself has no device node");
    static char codec[16384];
    expect(read_text("/proc/asound/card0/codec#0", codec, sizeof(codec)) == 0 &&
               contains(codec, "Vendor Id: 0x1af40012") && contains(codec, "Paths:"),
           "/proc/asound shows the codec and its paths");

    start = now_us();
    expect_eq(ioctl((int)fd, PCM_DRAIN, 0), 0, "drain waits for the queued audio");
    spent = now_us() - start;
    expect(spent > 30000 && spent < 200000, "for about one buffer");
    expect_eq(pcm_state((int)fd, &status), STATE_SETUP, "and leaves the stream set up");

    expect_eq(ioctl((int)fd, PCM_PREPARE, 0), 0, "the stream prepares again");
    expect_eq(writei((int)fd, tone, BUFFER), (s64)BUFFER, "a full buffer starts it");
    sleep_ms(300);
    expect_eq(writei((int)fd, tone, PERIOD), -EPIPE, "an underrun is reported");
    expect_eq(pcm_state((int)fd, &status), STATE_XRUN, "the stream is in XRUN");
    expect_eq(ioctl((int)fd, PCM_PREPARE, 0), 0, "and recovers with prepare");

    expect_eq(writei((int)fd, tone, BUFFER), (s64)BUFFER, "the buffer fills");
    expect_eq(ioctl((int)fd, PCM_PAUSE, (void *)1), 0, "the stream pauses");
    expect_eq(pcm_state((int)fd, &status), STATE_PAUSED, "and says so");
    u64 paused_at = status.hw_ptr;
    sleep_ms(120);
    pcm_state((int)fd, &status);
    expect_eq((s64)status.hw_ptr, (s64)paused_at, "the hardware pointer stays put");
    expect_eq(ioctl((int)fd, PCM_PAUSE, (void *)0), 0, "the stream resumes");
    for (int wait = 0; wait < 30; wait++) {
        sleep_ms(10);
        pcm_state((int)fd, &status);
        if (status.state != STATE_RUNNING || status.hw_ptr > paused_at) break;
    }
    expect(status.state == STATE_RUNNING && status.hw_ptr > paused_at, "and moves on");
    if (status.state != STATE_RUNNING || status.hw_ptr <= paused_at) {
        print("sound: after resume state ");
        print_number(status.state);
        print(" hw_ptr ");
        print_number((s64)status.hw_ptr);
        print(" paused at ");
        print_number((s64)paused_at);
        print("\n");
    }
    expect_eq(ioctl((int)fd, PCM_DROP, 0), 0, "drop stops it");

    expect_eq(set_thresholds((int)fd, 4U * BUFFER, BUFFER), 0, "start can be deferred");
    expect_eq(ioctl((int)fd, PCM_PREPARE, 0), 0, "prepare");
    expect_eq(writei((int)fd, tone, 2048), 2048, "frames are queued");
    u64 frames = 1024;
    expect_eq(ioctl((int)fd, PCM_REWIND, &frames), 0, "rewind answers");
    expect_eq((s64)frames, 1024, "and takes back 1024 frames");
    frames = 4096;
    expect_eq(ioctl((int)fd, PCM_REWIND, &frames), 0, "an oversized rewind answers");
    expect_eq((s64)frames, 1024, "and is limited to what is queued");
    frames = 512;
    expect_eq(ioctl((int)fd, PCM_FORWARD, &frames), 0, "forward answers");
    expect_eq((s64)frames, 512, "and skips 512 frames");
    pcm_state((int)fd, &status);
    expect_eq((s64)status.appl_ptr, 512, "the application pointer follows");
    expect_eq(ioctl((int)fd, PCM_DROP, 0), 0, "drop");
    close((int)fd);

    fd = open("/dev/snd/pcmC0D0p", O_RDWR | O_NONBLOCK, 0);
    expect(fd >= 0, "the device opens without blocking");
    if (fd < 0) return;
    expect_eq(configure((int)fd, ACCESS_MMAP, PERIOD, PERIODS), 0, "mmap access is configured");
    expect_eq(set_thresholds((int)fd, BUFFER, BUFFER), 0, "thresholds again");
    expect_eq(ioctl((int)fd, PCM_PREPARE, 0), 0, "prepare");
    u16 *ring = mmap_file(BUFFER * 4U, PROT_READ | PROT_WRITE, (int)fd, 0);
    expect((s64)ring > 0, "the ring maps");
    if ((s64)ring <= 0) return;
    for (u32 at = 0; at < BUFFER * 2U; at++) ring[at] = tone[at];
    struct sync_ptr sync;
    u8 *raw = (u8 *)&sync;
    for (u64 at = 0; at < sizeof(sync); at++) raw[at] = 0;
    sync.c.control.appl_ptr = BUFFER;
    sync.c.control.avail_min = PERIOD;
    expect_eq(ioctl((int)fd, PCM_SYNC_PTR, &sync), 0, "SYNC_PTR commits the frames");
    expect_eq((s64)sync.c.control.appl_ptr, (s64)BUFFER, "the pointer is taken");
    expect_eq(sync.s.status.state, STATE_PREPARED, "an mmap commit does not start the stream");
    expect_eq(ioctl((int)fd, PCM_START, 0), 0, "START runs it");
    s64 squeezed = writei((int)fd, tone, BUFFER);
    expect(squeezed == -EAGAIN || (squeezed >= 0 && squeezed < (s64)BUFFER),
           "a full ring takes only what has played");
    int refused = 0;
    for (int attempt = 0; attempt < 50 && !refused; attempt++) {
        s64 more = writei((int)fd, tone, BUFFER);
        if (more == -EAGAIN) refused = 1;
        else if (more < 0 || more >= (s64)BUFFER) break;
    }
    expect(refused, "and then refuses a non-blocking write");
    expect((ppoll1((int)fd, POLLOUT, 500) & POLLOUT) != 0, "poll reports room after a period");
    expect_eq(writei((int)fd, tone, PERIOD), (s64)PERIOD, "and the write then goes in");
    munmap(ring, BUFFER * 4U);
    close((int)fd);

    expect_eq(open("/dev/snd/pcmC0D0p", O_RDWR | O_APPEND, 0), -EBADFD,
              "append needs an open stream to join");
    fd = open("/dev/snd/pcmC0D0p", O_RDWR, 0);
    expect(fd >= 0, "the device opens for the timer test");
    if (fd < 0) return;
    s64 joined = open("/dev/snd/pcmC0D0p", O_RDWR | O_APPEND, 0);
    expect(joined >= 0, "a second opener joins with O_APPEND");
    expect_eq(configure((int)fd, ACCESS_RW, PERIOD, PERIODS), 0, "configured for the timer");
    expect_eq(set_thresholds((int)fd, BUFFER, BUFFER), 0, "thresholds for the timer");
    expect_eq(ioctl((int)fd, PCM_PREPARE, 0), 0, "prepared for the timer");
    if (joined >= 0)
        expect_eq(pcm_state((int)joined, &status), STATE_PREPARED,
                  "the joined opener shares the stream");

    s64 timer = open("/dev/snd/timer", O_RDONLY | O_NONBLOCK, 0);
    expect(timer >= 0, "the timer device opens");
    int version = 0;
    expect_eq(ioctl((int)timer, TIMER_PVERSION, &version), 0, "the timer answers its version");
    int tread = 1;
    expect_eq(ioctl((int)timer, TIMER_TREAD, &tread), 0, "timestamped reads are chosen");
    struct timer_select select;
    for (u64 at = 0; at < sizeof(select); at++) ((u8 *)&select)[at] = 0;
    select.id.dev_class = 3;
    expect_eq(ioctl((int)timer, TIMER_SELECT, &select), 0, "the playback timer is selected");
    struct timer_params tp;
    for (u64 at = 0; at < sizeof(tp); at++) ((u8 *)&tp)[at] = 0;
    tp.ticks = 1;
    tp.filter = 1U << 1;
    expect_eq(ioctl((int)timer, TIMER_PARAMS, &tp), 0, "ticks are requested");
    expect_eq(ioctl((int)timer, TIMER_START, 0), 0, "the timer starts");
    start = now_us();
    expect_eq(writei((int)fd, tone, RATE / 2U), (s64)(RATE / 2U), "half a second is played");
    unsigned ticks = 0;
    unsigned events = 0;
    struct timer_tread record;
    while (now_us() - start < 700000) {
        if (ppoll1((int)timer, POLLIN, 100) <= 0) continue;
        while (read((int)timer, &record, sizeof(record)) == (s64)sizeof(record)) {
            if (record.event == 1) {
                ticks += record.value;
                events++;
            }
        }
    }
    expect(ticks >= 18 && ticks <= 40, "the timer ticks once per period");
    expect(events > 0, "and wakes its poller");
    ioctl((int)timer, TIMER_STOP, 0);
    close((int)timer);
    if (joined >= 0) close((int)joined);
    ioctl((int)fd, PCM_DROP, 0);
    close((int)fd);
}

static u16 captured[RATE * 2];

static void capture(void) {
    expect_eq(open("/dev/snd/pcmC0D0c", O_RDWR, 0), -ENOENT, "card 0 has no capture device");
    s64 fd = open("/dev/snd/pcmC1D0c", O_RDWR, 0);
    expect(fd >= 0, "the second card captures");
    if (fd < 0) return;
    expect_eq(configure((int)fd, ACCESS_RW, PERIOD, PERIODS), 0, "capture takes 4 x 1024 frames");
    expect_eq(set_thresholds((int)fd, 1, BUFFER), 0, "capture thresholds");
    expect_eq(ioctl((int)fd, PCM_PREPARE, 0), 0, "capture prepares");
    s64 start = now_us();
    expect_eq(readi((int)fd, captured, RATE), (s64)RATE, "a blocking read of one second is filled");
    s64 spent = now_us() - start;
    expect(spent > 900000 && spent < 1150000, "at the hardware rate");
    struct pcm_status status;
    expect_eq(pcm_state((int)fd, &status), STATE_RUNNING, "capture runs");
    expect_eq(ioctl((int)fd, PCM_DROP, 0), 0, "capture stops");
    close((int)fd);
}

static void final_tone(void) {
    s64 fd = open("/dev/snd/pcmC0D0p", O_RDWR, 0);
    if (fd < 0) return;
    configure((int)fd, ACCESS_RW, PERIOD, PERIODS);
    set_thresholds((int)fd, BUFFER, BUFFER);
    ioctl((int)fd, PCM_PREPARE, 0);
    expect_eq(writei((int)fd, tone, 2U * RATE), (s64)(2U * RATE), "the reference tone is played");
    expect_eq(ioctl((int)fd, PCM_DRAIN, 0), 0, "and drained");
    close((int)fd);
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    make_pattern();
    s64 module = open("/snd_hda.ko", O_RDONLY, 0);
    expect(module >= 0, "the driver module is present");
    expect_eq(SYSCALL(SYS_finit_module, module, "", 0, 0, 0, 0), 0, "snd_hda loads");
    close((int)module);
    s64 probe = -1;
    for (int tries = 0; tries < 50 && probe < 0; tries++) {
        probe = open("/dev/snd/controlC0", O_RDONLY, 0);
        if (probe < 0) sleep_ms(100);
    }
    expect(probe >= 0, "the card appears");
    if (probe < 0) return;
    close((int)probe);
    probe = -1;
    for (int tries = 0; tries < 50 && probe < 0; tries++) {
        probe = open("/dev/snd/controlC1", O_RDONLY, 0);
        if (probe < 0) sleep_ms(100);
    }
    expect(probe >= 0, "the second controller is card 1");
    if (probe >= 0) close((int)probe);
    controls();
    playback();
    capture();
    final_tone();
}
