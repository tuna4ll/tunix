#include <stddef.h>
#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/devfs.h>
#include <tunix/devnum.h>
#include <tunix/errno.h>
#include <tunix/file.h>
#include <tunix/heap.h>
#include <tunix/kprintf.h>
#include <tunix/kstring.h>
#include <tunix/lock.h>
#include <tunix/module.h>
#include <tunix/pmm.h>
#include <tunix/process.h>
#include <tunix/sound.h>
#include <tunix/sysfs.h>
#include <tunix/time.h>
#include <tunix/usercopy.h>
#include <tunix/vfs.h>
#include <tunix/vmm.h>
#include <uapi/asound.h>

#define RING_MAX_PAGES   128U
#define BYTES_ALIGN      128U
#define REFINE_PASSES    64U
#define WAIT_TIMEOUT_NS  (10ULL * 1000ULL * 1000ULL * 1000ULL)
#define EVENT_QUEUE      64U
#define CONTROL_MAX      256U
#define NSEC             1000000000ULL
#define O_NONBLOCK_FLAG  04000U
#define POLL_INTERVAL_NS (250ULL * 1000ULL * 1000ULL)

_Static_assert(sizeof(struct snd_pcm_hw_params) == 608, "hw_params layout");
_Static_assert(sizeof(struct snd_pcm_sw_params) == 136, "sw_params layout");
_Static_assert(sizeof(struct snd_pcm_info) == 288, "pcm_info layout");
_Static_assert(sizeof(struct snd_pcm_status) == 152, "pcm_status layout");
_Static_assert(sizeof(struct snd_pcm_sync_ptr) == 136, "sync_ptr layout");
_Static_assert(sizeof(struct snd_pcm_channel_info) == 24, "channel_info layout");
_Static_assert(sizeof(struct snd_ctl_card_info) == 376, "card_info layout");
_Static_assert(sizeof(struct snd_ctl_elem_id) == 64, "elem_id layout");
_Static_assert(sizeof(struct snd_ctl_elem_list) == 80, "elem_list layout");
_Static_assert(sizeof(struct snd_ctl_elem_info) == 272, "elem_info layout");
_Static_assert(sizeof(struct snd_ctl_elem_value) == 1224, "elem_value layout");
_Static_assert(sizeof(struct snd_ctl_event) == 72, "ctl_event layout");

struct snd_card;

struct snd_stream {
    struct snd_card *card;
    const struct snd_pcm_ops *ops;
    int direction;
    int open;
    int state;
    int configured;
    unsigned generation;
    unsigned waiters;

    uint64_t ring_physical[RING_MAX_PAGES];
    uint8_t *ring_virtual[RING_MAX_PAGES];
    unsigned ring_pages;

    uint32_t access;
    uint32_t rate;
    uint32_t channels;
    uint32_t format;
    uint32_t sample_bits;
    uint32_t frame_bits;
    uint32_t frame_bytes;
    uint32_t msbits;
    uint32_t period_bytes;
    uint32_t buffer_bytes;
    uint64_t period_size;
    uint64_t buffer_size;
    uint64_t periods;

    uint64_t boundary;
    uint64_t avail_min;
    uint64_t start_threshold;
    uint64_t stop_threshold;
    uint64_t silence_threshold;
    uint64_t silence_size;
    uint64_t silence_start;
    uint64_t silence_filled;
    int tstamp_mode;
    int tstamp_type;

    uint64_t appl_ptr;
    uint64_t hw_ptr;
    uint64_t hw_frames;
    uint64_t avail_max;
    uint64_t wake_need;
    uint64_t trigger_ns;
    uint64_t update_ns;
    uint64_t xruns;
};

struct snd_ctl_subscriber {
    struct snd_ctl_subscriber *next;
    struct snd_card *card;
    int subscribed;
    unsigned head;
    unsigned count;
    struct snd_ctl_event events[EVENT_QUEUE];
};

struct snd_card {
    int present;
    int index;
    const struct snd_backend *backend;
    struct module *owner;
    struct lock lock;
    struct snd_stream streams[2];
    struct snd_ctl_subscriber *subscribers;
    unsigned control_opens;
    char id[16];
};

static struct lock cards_lock = LOCK_INITIALIZER("sound cards", LOCK_RANK_DEVICE - 1);
static struct snd_card cards[SND_MAX_CARDS];

static void copy_field(unsigned char *destination, size_t size, const char *text) {
    memset(destination, 0, size);
    if (!text) return;
    size_t index = 0;
    while (text[index] && index + 1U < size) {
        destination[index] = (unsigned char)text[index];
        index++;
    }
}

static uint64_t divide_up(uint64_t value, uint64_t divisor) {
    return divisor ? (value + divisor - 1U) / divisor : UINT32_MAX;
}

static int stream_running(const struct snd_stream *s) {
    return s->state == SNDRV_PCM_STATE_RUNNING || s->state == SNDRV_PCM_STATE_DRAINING;
}

static uint64_t stream_now(const struct snd_stream *s) {
    if (s->tstamp_type == SNDRV_PCM_TSTAMP_TYPE_GETTIMEOFDAY) return time_realtime_ns();
    return time_uptime_ns();
}

static void ns_to_timespec(uint64_t ns, struct snd_timespec *out) {
    out->tv_sec = (int64_t)(ns / NSEC);
    out->tv_nsec = (int64_t)(ns % NSEC);
}

static struct snd_card *card_of_minor(uint32_t minor, int *stream) {
    unsigned index = minor / 32U;
    unsigned kind = minor % 32U;
    if (index >= SND_MAX_CARDS || !cards[index].present) return NULL;
    if (stream) {
        if (kind == 16U) *stream = SND_PLAYBACK;
        else if (kind == 24U) *stream = SND_CAPTURE;
        else return NULL;
    } else if (kind != 0U) {
        return NULL;
    }
    return &cards[index];
}

static int ring_reserve(struct snd_stream *s, unsigned pages) {
    if (pages > RING_MAX_PAGES) return -ENOMEM;
    while (s->ring_pages < pages) {
        void *physical = pmm_alloc_page();
        if (!physical) return -ENOMEM;
        uint8_t *virtual_address = vmm_phys_to_virt((uint64_t)physical);
        if (!virtual_address) {
            pmm_free_page(physical);
            return -ENOMEM;
        }
        memset(virtual_address, 0, 4096);
        s->ring_physical[s->ring_pages] = (uint64_t)physical;
        s->ring_virtual[s->ring_pages] = virtual_address;
        s->ring_pages++;
    }
    return 0;
}

static void ring_release(struct snd_stream *s) {
    for (unsigned index = 0; index < s->ring_pages; index++)
        pmm_free_page((void *)s->ring_physical[index]);
    s->ring_pages = 0;
}

static void ring_zero(struct snd_stream *s, uint64_t offset, uint64_t size) {
    while (size) {
        uint64_t page = offset / 4096U;
        uint64_t within = offset % 4096U;
        uint64_t chunk = 4096U - within;
        if (chunk > size) chunk = size;
        if (page >= s->ring_pages) return;
        memset(s->ring_virtual[page] + within, 0, chunk);
        offset += chunk;
        size -= chunk;
    }
}

static void ring_zero_frames(struct snd_stream *s, uint64_t frame, uint64_t frames) {
    if (!s->buffer_size) return;
    if (frames > s->buffer_size) frames = s->buffer_size;
    frame %= s->buffer_size;
    while (frames) {
        uint64_t run = s->buffer_size - frame;
        if (run > frames) run = frames;
        ring_zero(s, frame * s->frame_bytes, run * s->frame_bytes);
        frame = (frame + run) % s->buffer_size;
        frames -= run;
    }
}

static int ring_copy_user(struct snd_stream *s, uint64_t offset, uint64_t user, uint64_t size,
                          int to_ring, int kernel) {
    while (size) {
        uint64_t page = offset / 4096U;
        uint64_t within = offset % 4096U;
        uint64_t chunk = 4096U - within;
        if (chunk > size) chunk = size;
        if (page >= s->ring_pages) return -EFAULT;
        uint8_t *at = s->ring_virtual[page] + within;
        int status = 0;
        if (kernel && to_ring) memcpy(at, (const void *)(uintptr_t)user, chunk);
        else if (kernel) memcpy((void *)(uintptr_t)user, at, chunk);
        else status = to_ring ? copy_from_user(at, user, chunk) : copy_to_user(user, at, chunk);
        if (status != 0) return -EFAULT;
        offset += chunk;
        user += chunk;
        size -= chunk;
    }
    return 0;
}

static int64_t playback_avail(const struct snd_stream *s) {
    int64_t avail = (int64_t)(s->hw_ptr + s->buffer_size) - (int64_t)s->appl_ptr;
    if (avail < 0) avail += (int64_t)s->boundary;
    else if ((uint64_t)avail >= s->boundary) avail -= (int64_t)s->boundary;
    return avail;
}

static int64_t capture_avail(const struct snd_stream *s) {
    int64_t avail = (int64_t)s->hw_ptr - (int64_t)s->appl_ptr;
    if (avail < 0) avail += (int64_t)s->boundary;
    return avail;
}

static int64_t stream_avail(const struct snd_stream *s) {
    return s->direction == SND_PLAYBACK ? playback_avail(s) : capture_avail(s);
}

static int64_t stream_hw_avail(const struct snd_stream *s) {
    return (int64_t)s->buffer_size - stream_avail(s);
}

static void stream_wake(struct snd_stream *s) {
    if (s->waiters) process_wake_all(s);
}

static void stream_trigger(struct snd_stream *s, int command) {
    if (s->ops && s->ops->trigger) (void)s->ops->trigger(s->card->backend->context, command);
}

static void stream_set_xrun(struct snd_stream *s) {
    stream_trigger(s, SND_TRIGGER_STOP);
    s->state = SNDRV_PCM_STATE_XRUN;
    s->xruns++;
    s->trigger_ns = stream_now(s);
}

static void silence_threshold_fill(struct snd_stream *s) {
    if (!s->silence_size || s->silence_size >= s->boundary) return;
    int64_t noise = stream_hw_avail(s);
    if (noise < 0) noise = 0;
    if ((uint64_t)noise >= s->silence_threshold) return;
    uint64_t start = s->appl_ptr;
    if (s->silence_filled && s->silence_start == s->appl_ptr) start += s->silence_filled;
    else s->silence_filled = 0;
    uint64_t room = s->buffer_size - (uint64_t)noise;
    if (s->silence_filled >= room) return;
    uint64_t fill = s->silence_size;
    if (fill > room - s->silence_filled) fill = room - s->silence_filled;
    ring_zero_frames(s, start, fill);
    s->silence_start = s->appl_ptr;
    s->silence_filled += fill;
}

static void stream_update(struct snd_stream *s) {
    if (!s->configured || !s->ops || !s->buffer_size) return;
    if (!stream_running(s)) return;
    if (s->direction == SND_CAPTURE && s->state == SNDRV_PCM_STATE_DRAINING) return;

    uint64_t now = time_uptime_ns();
    uint32_t position = s->ops->position(s->card->backend->context);
    if (position >= s->buffer_bytes) position = 0;
    uint64_t frames = position / s->frame_bytes;
    uint64_t offset = s->hw_ptr % s->buffer_size;
    uint64_t delta = (frames + s->buffer_size - offset) % s->buffer_size;
    if (s->update_ns && now > s->update_ns) {
        uint64_t elapsed = now - s->update_ns;
        uint64_t expected = elapsed / 1000U * s->rate / 1000000U;
        if (delta > s->buffer_size / 2U && expected + s->period_size < delta) return;
        if (expected > delta + s->buffer_size / 2U)
            delta += (expected - delta + s->buffer_size / 2U) / s->buffer_size * s->buffer_size;
    }
    s->update_ns = now;
    if (delta) {
        if (s->direction == SND_PLAYBACK && s->silence_size >= s->boundary)
            ring_zero_frames(s, s->hw_ptr, delta);
        s->hw_ptr = (s->hw_ptr + delta) % s->boundary;
        s->hw_frames += delta;
    }

    int64_t avail = stream_avail(s);
    if (avail > 0 && (uint64_t)avail > s->avail_max) s->avail_max = (uint64_t)avail;

    if (s->direction == SND_PLAYBACK && s->state == SNDRV_PCM_STATE_DRAINING) {
        if (avail >= (int64_t)s->buffer_size) {
            stream_trigger(s, SND_TRIGGER_STOP);
            s->state = SNDRV_PCM_STATE_SETUP;
            stream_wake(s);
        }
        return;
    }
    if (s->stop_threshold < s->boundary && avail >= (int64_t)s->stop_threshold) {
        stream_set_xrun(s);
        stream_wake(s);
        return;
    }
    if (s->direction == SND_PLAYBACK) silence_threshold_fill(s);
    if (avail >= (int64_t)s->wake_need) stream_wake(s);
}

static int stream_start(struct snd_stream *s) {
    if (s->state == SNDRV_PCM_STATE_RUNNING) return 0;
    if (s->state != SNDRV_PCM_STATE_PREPARED) return -EBADFD;
    if (s->direction == SND_PLAYBACK && stream_hw_avail(s) <= 0) return -EPIPE;
    if (s->ops->trigger(s->card->backend->context, SND_TRIGGER_START) != 0) return -EIO;
    s->state = SNDRV_PCM_STATE_RUNNING;
    s->trigger_ns = stream_now(s);
    s->update_ns = time_uptime_ns();
    return 0;
}

static void stream_stop(struct snd_stream *s, int state) {
    if (stream_running(s) || s->state == SNDRV_PCM_STATE_PAUSED)
        stream_trigger(s, SND_TRIGGER_STOP);
    if (s->state != state) s->trigger_ns = stream_now(s);
    s->state = state;
    s->generation++;
    stream_wake(s);
}

static void ring_clear(struct snd_stream *s) {
    for (unsigned index = 0; index < s->ring_pages; index++)
        memset(s->ring_virtual[index], 0, 4096);
}

static int stream_prepare(struct snd_stream *s) {
    if (!s->configured) return -EBADFD;
    if (s->state == SNDRV_PCM_STATE_OPEN || s->state == SNDRV_PCM_STATE_DISCONNECTED)
        return -EBADFD;
    stream_stop(s, SNDRV_PCM_STATE_SETUP);
    if (s->ops->prepare && s->ops->prepare(s->card->backend->context) != 0) return -EIO;
    ring_clear(s);
    s->appl_ptr = 0;
    s->hw_ptr = 0;
    s->hw_frames = 0;
    s->silence_start = 0;
    s->silence_filled = 0;
    s->avail_max = 0;
    s->update_ns = 0;
    s->state = SNDRV_PCM_STATE_PREPARED;
    return 0;
}

struct interval {
    uint32_t min;
    uint32_t max;
    uint8_t openmin;
    uint8_t openmax;
    uint8_t integer;
    uint8_t empty;
};

struct refine_state {
    uint32_t masks[SNDRV_PCM_HW_PARAM_MASK_COUNT][SNDRV_MASK_WORDS];
    struct interval iv[SNDRV_PCM_HW_PARAM_INTERVAL_COUNT];
};

#define IV(state, var) (&(state)->iv[(var) - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL])

static void interval_load(struct interval *out, const struct snd_interval *in) {
    out->min = in->min;
    out->max = in->max;
    out->openmin = (uint8_t)in->openmin;
    out->openmax = (uint8_t)in->openmax;
    out->integer = (uint8_t)in->integer;
    out->empty = (uint8_t)in->empty;
}

static void interval_store(struct snd_interval *out, const struct interval *in) {
    out->min = in->min;
    out->max = in->max;
    out->openmin = in->openmin;
    out->openmax = in->openmax;
    out->integer = in->integer;
    out->empty = in->empty;
}

static int interval_checkempty(const struct interval *i) {
    return i->min > i->max || (i->min == i->max && (i->openmin || i->openmax));
}

static int interval_refine(struct interval *i, const struct interval *v) {
    if (i->empty) return -EINVAL;
    if (v->empty) {
        i->empty = 1;
        return -EINVAL;
    }
    int changed = 0;
    if (i->min < v->min) {
        i->min = v->min;
        i->openmin = v->openmin;
        changed = 1;
    } else if (i->min == v->min && !i->openmin && v->openmin) {
        i->openmin = 1;
        changed = 1;
    }
    if (i->max > v->max) {
        i->max = v->max;
        i->openmax = v->openmax;
        changed = 1;
    } else if (i->max == v->max && !i->openmax && v->openmax) {
        i->openmax = 1;
        changed = 1;
    }
    if (!i->integer && v->integer) {
        i->integer = 1;
        changed = 1;
    }
    if (i->integer) {
        if (i->openmin) {
            i->min++;
            i->openmin = 0;
        }
        if (i->openmax) {
            i->max--;
            i->openmax = 0;
        }
    } else if (!i->openmin && !i->openmax && i->min == i->max) {
        i->integer = 1;
    }
    if (interval_checkempty(i)) {
        i->empty = 1;
        return -EINVAL;
    }
    return changed;
}

static int interval_minmax(struct interval *i, uint32_t min, uint32_t max) {
    struct interval v = {.min = min, .max = max};
    return interval_refine(i, &v);
}

static int interval_setinteger(struct interval *i) {
    struct interval v = {.min = 0, .max = UINT32_MAX, .integer = 1};
    return interval_refine(i, &v);
}

static int interval_step(struct interval *i, uint32_t step) {
    if (i->empty) return -EINVAL;
    int changed = 0;
    uint32_t remainder = i->min % step;
    if (remainder || i->openmin) {
        uint64_t raised = (uint64_t)i->min - remainder + step;
        i->min = raised > UINT32_MAX ? UINT32_MAX : (uint32_t)raised;
        i->openmin = 0;
        changed = 1;
    }
    remainder = i->max % step;
    if (remainder || i->openmax) {
        i->max -= remainder;
        i->openmax = 0;
        changed = 1;
    }
    if (interval_checkempty(i)) {
        i->empty = 1;
        return -EINVAL;
    }
    return changed;
}

static uint32_t mul_sat(uint32_t a, uint32_t b) {
    uint64_t value = (uint64_t)a * b;
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static uint32_t div32(uint32_t a, uint32_t b, uint32_t *remainder) {
    if (!b) {
        *remainder = 0;
        return UINT32_MAX;
    }
    *remainder = a % b;
    return a / b;
}

static uint32_t muldiv32(uint32_t a, uint32_t b, uint32_t c, uint32_t *remainder) {
    uint64_t n = (uint64_t)a * b;
    if (!c) {
        *remainder = 0;
        return UINT32_MAX;
    }
    uint64_t q = n / c;
    if (q > UINT32_MAX) {
        *remainder = 0;
        return UINT32_MAX;
    }
    *remainder = (uint32_t)(n % c);
    return (uint32_t)q;
}

static void interval_mul(const struct interval *a, const struct interval *b, struct interval *c) {
    memset(c, 0, sizeof(*c));
    if (a->empty || b->empty) {
        c->empty = 1;
        return;
    }
    c->min = mul_sat(a->min, b->min);
    c->openmin = a->openmin || b->openmin;
    c->max = mul_sat(a->max, b->max);
    c->openmax = a->openmax || b->openmax;
    c->integer = a->integer && b->integer;
}

static void interval_div(const struct interval *a, const struct interval *b, struct interval *c) {
    uint32_t r;
    memset(c, 0, sizeof(*c));
    if (a->empty || b->empty) {
        c->empty = 1;
        return;
    }
    c->min = div32(a->min, b->max, &r);
    c->openmin = r || a->openmin || b->openmax;
    if (b->min > 0) {
        c->max = div32(a->max, b->min, &r);
        if (r) {
            c->max++;
            c->openmax = 1;
        } else {
            c->openmax = a->openmax || b->openmin;
        }
    } else {
        c->max = UINT32_MAX;
        c->openmax = 0;
    }
}

static void interval_muldivk(const struct interval *a, const struct interval *b, uint32_t k,
                             struct interval *c) {
    uint32_t r;
    memset(c, 0, sizeof(*c));
    if (a->empty || b->empty) {
        c->empty = 1;
        return;
    }
    c->min = muldiv32(a->min, b->min, k, &r);
    c->openmin = r || a->openmin || b->openmin;
    c->max = muldiv32(a->max, b->max, k, &r);
    if (r) {
        c->max++;
        c->openmax = 1;
    } else {
        c->openmax = a->openmax || b->openmax;
    }
}

static void interval_mulkdiv(const struct interval *a, uint32_t k, const struct interval *b,
                             struct interval *c) {
    uint32_t r;
    memset(c, 0, sizeof(*c));
    if (a->empty || b->empty) {
        c->empty = 1;
        return;
    }
    c->min = muldiv32(a->min, k, b->max, &r);
    c->openmin = r || a->openmin || b->openmax;
    if (b->min > 0) {
        c->max = muldiv32(a->max, k, b->min, &r);
        if (r) {
            c->max++;
            c->openmax = 1;
        } else {
            c->openmax = a->openmax || b->openmin;
        }
    } else {
        c->max = UINT32_MAX;
        c->openmax = 0;
    }
}

static int interval_single(const struct interval *i) {
    return !i->empty && (i->min == i->max || (i->min + 1U == i->max && (i->openmin || i->openmax)));
}

static uint32_t interval_value(const struct interval *i) {
    if (i->openmin && !i->integer) return i->max;
    if (i->openmin) return i->min + 1U;
    return i->min;
}

static int interval_first(struct interval *i) {
    if (interval_single(i)) return 0;
    uint32_t last_max = i->max;
    i->max = i->min;
    if (i->openmin) i->max++;
    i->openmax = i->openmax && i->max >= last_max;
    return 1;
}

static int interval_last(struct interval *i) {
    if (interval_single(i)) return 0;
    uint32_t last_min = i->min;
    i->min = i->max;
    if (i->openmax) i->min--;
    i->openmin = i->openmin && i->min <= last_min;
    return 1;
}

static int mask_test(const uint32_t *mask, unsigned bit) {
    if (bit >= SNDRV_MASK_BITS) return 0;
    return (mask[bit / 32U] >> (bit % 32U)) & 1U;
}

static int mask_empty(const uint32_t *mask) {
    for (unsigned word = 0; word < SNDRV_MASK_WORDS; word++)
        if (mask[word]) return 0;
    return 1;
}

static int mask_and(uint32_t *mask, const uint32_t *allowed) {
    int changed = 0;
    for (unsigned word = 0; word < SNDRV_MASK_WORDS; word++) {
        uint32_t value = mask[word] & allowed[word];
        if (value != mask[word]) changed = 1;
        mask[word] = value;
    }
    return changed;
}

static unsigned mask_first(const uint32_t *mask) {
    for (unsigned bit = 0; bit < SNDRV_MASK_BITS; bit++)
        if (mask_test(mask, bit)) return bit;
    return SNDRV_MASK_BITS;
}

static int mask_single(const uint32_t *mask) {
    unsigned found = 0;
    for (unsigned word = 0; word < SNDRV_MASK_WORDS; word++)
        found += (unsigned)__builtin_popcount(mask[word]);
    return found == 1U;
}

static int mask_keep_first(uint32_t *mask) {
    if (mask_single(mask)) return 0;
    unsigned bit = mask_first(mask);
    memset(mask, 0, SNDRV_MASK_WORDS * sizeof(mask[0]));
    if (bit < SNDRV_MASK_BITS) mask[bit / 32U] = 1U << (bit % 32U);
    return 1;
}

static uint32_t format_width(unsigned format) {
    switch (format) {
    case SNDRV_PCM_FORMAT_S16_LE: return 16;
    case SNDRV_PCM_FORMAT_S32_LE: return 32;
    default:                      return 0;
    }
}

static int apply(struct interval *target, const struct interval *rule) {
    return interval_refine(target, rule);
}

static int rule_formats(struct refine_state *st) {
    struct interval *bits = IV(st, SNDRV_PCM_HW_PARAM_SAMPLE_BITS);
    uint32_t *formats = st->masks[SNDRV_PCM_HW_PARAM_FORMAT];
    int changed = 0;
    for (unsigned format = 0; format < 64U; format++) {
        if (!mask_test(formats, format)) continue;
        uint32_t width = format_width(format);
        int fits = width && width >= bits->min + (bits->openmin ? 1U : 0U) &&
            width <= bits->max - (bits->openmax ? 1U : 0U);
        if (!fits) {
            formats[format / 32U] &= ~(1U << (format % 32U));
            changed = 1;
        }
    }
    if (mask_empty(formats)) return -EINVAL;
    uint32_t low = UINT32_MAX;
    uint32_t high = 0;
    for (unsigned format = 0; format < 64U; format++) {
        if (!mask_test(formats, format)) continue;
        uint32_t width = format_width(format);
        if (width < low) low = width;
        if (width > high) high = width;
    }
    int refined = interval_minmax(bits, low, high);
    if (refined < 0) return refined;
    return changed | refined;
}

static int rule_rate_list(struct refine_state *st, const struct snd_hardware *hw) {
    if (!hw->rate_count) return 0;
    struct interval *rate = IV(st, SNDRV_PCM_HW_PARAM_RATE);
    uint32_t low = 0;
    uint32_t high = 0;
    int found = 0;
    for (unsigned index = 0; index < hw->rate_count; index++) {
        uint32_t value = hw->rates[index];
        if (value < rate->min || (value == rate->min && rate->openmin)) continue;
        if (value > rate->max || (value == rate->max && rate->openmax)) continue;
        if (!found || value < low) low = value;
        if (!found || value > high) high = value;
        found = 1;
    }
    if (!found) {
        rate->empty = 1;
        return -EINVAL;
    }
    struct interval v = {.min = low, .max = high, .integer = 1};
    return interval_refine(rate, &v);
}

#define RULE(expression) \
    do { \
        struct interval t; \
        expression; \
        int result = apply(target, &t); \
        if (result < 0) return result; \
        changed |= result; \
    } while (0)

static int refine_pass(struct refine_state *st, const struct snd_hardware *hw) {
    int changed = 0;
    struct interval *sample_bits = IV(st, SNDRV_PCM_HW_PARAM_SAMPLE_BITS);
    struct interval *frame_bits = IV(st, SNDRV_PCM_HW_PARAM_FRAME_BITS);
    struct interval *channels = IV(st, SNDRV_PCM_HW_PARAM_CHANNELS);
    struct interval *rate = IV(st, SNDRV_PCM_HW_PARAM_RATE);
    struct interval *period_time = IV(st, SNDRV_PCM_HW_PARAM_PERIOD_TIME);
    struct interval *period_size = IV(st, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
    struct interval *period_bytes = IV(st, SNDRV_PCM_HW_PARAM_PERIOD_BYTES);
    struct interval *periods = IV(st, SNDRV_PCM_HW_PARAM_PERIODS);
    struct interval *buffer_time = IV(st, SNDRV_PCM_HW_PARAM_BUFFER_TIME);
    struct interval *buffer_size = IV(st, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
    struct interval *buffer_bytes = IV(st, SNDRV_PCM_HW_PARAM_BUFFER_BYTES);
    struct interval *target;

    int result = rule_formats(st);
    if (result < 0) return result;
    changed |= result;

    target = sample_bits;
    RULE(interval_div(frame_bits, channels, &t));
    target = frame_bits;
    RULE(interval_mul(sample_bits, channels, &t));
    RULE(interval_mulkdiv(period_bytes, 8, period_size, &t));
    RULE(interval_mulkdiv(buffer_bytes, 8, buffer_size, &t));
    target = channels;
    RULE(interval_div(frame_bits, sample_bits, &t));
    target = rate;
    RULE(interval_mulkdiv(period_size, 1000000U, period_time, &t));
    RULE(interval_mulkdiv(buffer_size, 1000000U, buffer_time, &t));
    result = rule_rate_list(st, hw);
    if (result < 0) return result;
    changed |= result;
    target = periods;
    RULE(interval_div(buffer_size, period_size, &t));
    target = period_size;
    RULE(interval_div(buffer_size, periods, &t));
    RULE(interval_mulkdiv(period_bytes, 8, frame_bits, &t));
    RULE(interval_muldivk(period_time, rate, 1000000U, &t));
    target = buffer_size;
    RULE(interval_mul(period_size, periods, &t));
    RULE(interval_mulkdiv(buffer_bytes, 8, frame_bits, &t));
    RULE(interval_muldivk(buffer_time, rate, 1000000U, &t));
    target = period_bytes;
    RULE(interval_muldivk(period_size, frame_bits, 8, &t));
    target = buffer_bytes;
    RULE(interval_muldivk(buffer_size, frame_bits, 8, &t));
    target = period_time;
    RULE(interval_mulkdiv(period_size, 1000000U, rate, &t));
    target = buffer_time;
    RULE(interval_mulkdiv(buffer_size, 1000000U, rate, &t));

    result = interval_step(period_bytes, BYTES_ALIGN);
    if (result < 0) return result;
    changed |= result;
    result = interval_step(buffer_bytes, BYTES_ALIGN);
    if (result < 0) return result;
    changed |= result;
    return changed;
}

static uint32_t stream_ring_limit(const struct snd_stream *s) {
    uint32_t limit = RING_MAX_PAGES * 4096U;
    uint32_t hw = s->ops->hardware.buffer_bytes_max;
    if (hw && hw < limit) limit = hw;
    return limit;
}

static int refine_state_run(const struct snd_stream *s, struct refine_state *st) {
    const struct snd_hardware *hw = &s->ops->hardware;
    uint32_t allowed[SNDRV_MASK_WORDS];

    memset(allowed, 0, sizeof(allowed));
    allowed[0] =
        (1U << SNDRV_PCM_ACCESS_MMAP_INTERLEAVED) | (1U << SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    (void)mask_and(st->masks[SNDRV_PCM_HW_PARAM_ACCESS], allowed);
    memset(allowed, 0, sizeof(allowed));
    allowed[0] = hw->formats;
    (void)mask_and(st->masks[SNDRV_PCM_HW_PARAM_FORMAT], allowed);
    memset(allowed, 0, sizeof(allowed));
    allowed[0] = 1U << SNDRV_PCM_SUBFORMAT_STD;
    (void)mask_and(st->masks[SNDRV_PCM_HW_PARAM_SUBFORMAT], allowed);
    for (unsigned index = 0; index < SNDRV_PCM_HW_PARAM_MASK_COUNT; index++)
        if (mask_empty(st->masks[index])) return -EINVAL;

    uint32_t rate_min = UINT32_MAX;
    uint32_t rate_max = 0;
    for (unsigned index = 0; index < hw->rate_count; index++) {
        if (hw->rates[index] < rate_min) rate_min = hw->rates[index];
        if (hw->rates[index] > rate_max) rate_max = hw->rates[index];
    }
    if (!hw->rate_count) return -EINVAL;

    if (interval_minmax(IV(st, SNDRV_PCM_HW_PARAM_CHANNELS), hw->channels_min, hw->channels_max) <
            0 ||
        interval_minmax(IV(st, SNDRV_PCM_HW_PARAM_RATE), rate_min, rate_max) < 0 ||
        interval_minmax(IV(st, SNDRV_PCM_HW_PARAM_PERIODS), hw->periods_min, hw->periods_max) < 0 ||
        interval_minmax(IV(st, SNDRV_PCM_HW_PARAM_PERIOD_BYTES), hw->period_bytes_min,
                        hw->period_bytes_max) < 0 ||
        interval_minmax(IV(st, SNDRV_PCM_HW_PARAM_BUFFER_BYTES), hw->period_bytes_min,
                        stream_ring_limit(s)) < 0)
        return -EINVAL;
    static const unsigned integers[] = {
        SNDRV_PCM_HW_PARAM_SAMPLE_BITS, SNDRV_PCM_HW_PARAM_FRAME_BITS,
        SNDRV_PCM_HW_PARAM_CHANNELS,    SNDRV_PCM_HW_PARAM_PERIODS,
        SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_BUFFER_BYTES};
    for (unsigned index = 0; index < sizeof(integers) / sizeof(integers[0]); index++)
        if (interval_setinteger(IV(st, integers[index])) < 0) return -EINVAL;

    for (unsigned pass = 0; pass < REFINE_PASSES; pass++) {
        int changed = refine_pass(st, hw);
        if (changed < 0) return changed;
        if (!changed) break;
    }
    for (unsigned index = 0; index < SNDRV_PCM_HW_PARAM_INTERVAL_COUNT; index++)
        if (st->iv[index].empty) return -EINVAL;
    return 0;
}

static void refine_state_load(struct refine_state *st, const struct snd_pcm_hw_params *params) {
    for (unsigned index = 0; index < SNDRV_PCM_HW_PARAM_MASK_COUNT; index++)
        memcpy(st->masks[index], params->masks[index].bits, sizeof(st->masks[index]));
    for (unsigned index = 0; index < SNDRV_PCM_HW_PARAM_INTERVAL_COUNT; index++)
        interval_load(&st->iv[index], &params->intervals[index]);
}

static void refine_state_store(struct snd_pcm_hw_params *params, const struct refine_state *st) {
    for (unsigned index = 0; index < SNDRV_PCM_HW_PARAM_MASK_COUNT; index++)
        memcpy(params->masks[index].bits, st->masks[index], sizeof(st->masks[index]));
    for (unsigned index = 0; index < SNDRV_PCM_HW_PARAM_INTERVAL_COUNT; index++)
        interval_store(&params->intervals[index], &st->iv[index]);
}

static void refine_finish(const struct snd_stream *s, struct snd_pcm_hw_params *params,
                          const struct snd_pcm_hw_params *before, const struct refine_state *st) {
    const struct snd_hardware *hw = &s->ops->hardware;
    refine_state_store(params, st);
    params->info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_INTERLEAVED |
        SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_PAUSE;
    const uint32_t *formats = st->masks[SNDRV_PCM_HW_PARAM_FORMAT];
    const struct interval *bits = IV(st, SNDRV_PCM_HW_PARAM_SAMPLE_BITS);
    params->msbits = 0;
    if (mask_single(formats)) {
        unsigned format = mask_first(formats);
        params->msbits = format == SNDRV_PCM_FORMAT_S32_LE && hw->msbits_s32 ? hw->msbits_s32
                                                                             : format_width(format);
    } else if (interval_single(bits)) {
        params->msbits = interval_value(bits);
    }
    const struct interval *rate = IV(st, SNDRV_PCM_HW_PARAM_RATE);
    if (interval_single(rate)) {
        params->rate_num = interval_value(rate);
        params->rate_den = 1;
    }
    params->fifo_size = hw->fifo_frames;

    uint32_t changed = 0;
    for (unsigned index = 0; index < SNDRV_PCM_HW_PARAM_MASK_COUNT; index++)
        if (memcmp(before->masks[index].bits, params->masks[index].bits,
                   sizeof(params->masks[index].bits)) != 0)
            changed |= 1U << (SNDRV_PCM_HW_PARAM_FIRST_MASK + index);
    for (unsigned index = 0; index < SNDRV_PCM_HW_PARAM_INTERVAL_COUNT; index++) {
        const struct snd_interval *was = &before->intervals[index];
        const struct snd_interval *now = &params->intervals[index];
        if (was->min != now->min || was->max != now->max || was->openmin != now->openmin ||
            was->openmax != now->openmax || was->integer != now->integer ||
            was->empty != now->empty)
            changed |= 1U << (SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + index);
    }
    params->cmask = changed;
    params->rmask = 0;
}

static int pcm_hw_refine(const struct snd_stream *s, struct snd_pcm_hw_params *params) {
    struct snd_pcm_hw_params before = *params;
    struct refine_state st;
    refine_state_load(&st, params);
    int status = refine_state_run(s, &st);
    if (status < 0) return status;
    refine_finish(s, params, &before, &st);
    return 0;
}

static int refine_choose(const struct snd_stream *s, struct refine_state *st) {
    static const unsigned order[] = {SNDRV_PCM_HW_PARAM_ACCESS,      SNDRV_PCM_HW_PARAM_FORMAT,
                                     SNDRV_PCM_HW_PARAM_SUBFORMAT,   SNDRV_PCM_HW_PARAM_CHANNELS,
                                     SNDRV_PCM_HW_PARAM_RATE,        SNDRV_PCM_HW_PARAM_PERIOD_TIME,
                                     SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_TICK_TIME};
    for (unsigned index = 0; index < sizeof(order) / sizeof(order[0]); index++) {
        unsigned var = order[index];
        int changed;
        if (var <= SNDRV_PCM_HW_PARAM_LAST_MASK) changed = mask_keep_first(st->masks[var]);
        else if (var == SNDRV_PCM_HW_PARAM_BUFFER_SIZE) changed = interval_last(IV(st, var));
        else changed = interval_first(IV(st, var));
        if (!changed) continue;
        int status = refine_state_run(s, st);
        if (status < 0) return status;
    }
    return 0;
}

static int pcm_hw_params(struct snd_stream *s, struct snd_pcm_hw_params *params) {
    if (s->state != SNDRV_PCM_STATE_OPEN && s->state != SNDRV_PCM_STATE_SETUP &&
        s->state != SNDRV_PCM_STATE_PREPARED)
        return -EBADFD;
    struct snd_pcm_hw_params before = *params;
    struct refine_state st;
    refine_state_load(&st, params);
    int status = refine_state_run(s, &st);
    if (status < 0) return status;
    status = refine_choose(s, &st);
    if (status < 0) return status;
    refine_finish(s, params, &before, &st);

    if (!mask_single(st.masks[SNDRV_PCM_HW_PARAM_ACCESS]) ||
        !mask_single(st.masks[SNDRV_PCM_HW_PARAM_FORMAT]))
        return -EINVAL;
    static const unsigned needed[] = {
        SNDRV_PCM_HW_PARAM_CHANNELS,     SNDRV_PCM_HW_PARAM_RATE,
        SNDRV_PCM_HW_PARAM_PERIOD_SIZE,  SNDRV_PCM_HW_PARAM_PERIODS,
        SNDRV_PCM_HW_PARAM_BUFFER_SIZE,  SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
        SNDRV_PCM_HW_PARAM_BUFFER_BYTES, SNDRV_PCM_HW_PARAM_FRAME_BITS};
    for (unsigned index = 0; index < sizeof(needed) / sizeof(needed[0]); index++)
        if (!interval_single(IV(&st, needed[index]))) return -EINVAL;

    uint32_t access = mask_first(st.masks[SNDRV_PCM_HW_PARAM_ACCESS]);
    uint32_t format = mask_first(st.masks[SNDRV_PCM_HW_PARAM_FORMAT]);
    uint32_t channels = interval_value(IV(&st, SNDRV_PCM_HW_PARAM_CHANNELS));
    uint32_t rate = interval_value(IV(&st, SNDRV_PCM_HW_PARAM_RATE));
    uint32_t period_size = interval_value(IV(&st, SNDRV_PCM_HW_PARAM_PERIOD_SIZE));
    uint32_t periods = interval_value(IV(&st, SNDRV_PCM_HW_PARAM_PERIODS));
    uint32_t buffer_size = interval_value(IV(&st, SNDRV_PCM_HW_PARAM_BUFFER_SIZE));
    uint32_t period_bytes = interval_value(IV(&st, SNDRV_PCM_HW_PARAM_PERIOD_BYTES));
    uint32_t buffer_bytes = interval_value(IV(&st, SNDRV_PCM_HW_PARAM_BUFFER_BYTES));
    uint32_t frame_bits = interval_value(IV(&st, SNDRV_PCM_HW_PARAM_FRAME_BITS));
    if (!period_size || !buffer_size || frame_bits % 8U || !channels ||
        (uint64_t)period_size * periods != buffer_size ||
        (uint64_t)buffer_size * (frame_bits / 8U) != buffer_bytes ||
        buffer_bytes > stream_ring_limit(s))
        return -EINVAL;

    stream_stop(s, SNDRV_PCM_STATE_OPEN);
    s->configured = 0;
    unsigned pages = (unsigned)divide_up(buffer_bytes, 4096U);
    status = ring_reserve(s, pages);
    if (status < 0) return status;
    struct snd_stream_format stream = {
        .rate = rate, .channels = channels, .format = format, .msbits = params->msbits};
    if (s->ops->configure(s->card->backend->context, &stream, s->ring_physical, pages, buffer_bytes,
                          period_bytes) != 0)
        return -EINVAL;

    s->access = access;
    s->rate = rate;
    s->channels = channels;
    s->format = format;
    s->sample_bits = format_width(format);
    s->msbits = params->msbits;
    s->frame_bits = frame_bits;
    s->frame_bytes = frame_bits / 8U;
    s->period_size = period_size;
    s->periods = periods;
    s->buffer_size = buffer_size;
    s->period_bytes = period_bytes;
    s->buffer_bytes = buffer_bytes;
    s->boundary = buffer_size;
    while (s->boundary * 2U <= (uint64_t)INT64_MAX - buffer_size) s->boundary *= 2U;
    s->avail_min = period_size;
    s->wake_need = period_size;
    s->start_threshold = 1;
    s->stop_threshold = buffer_size;
    s->silence_threshold = 0;
    s->silence_size = 0;
    s->tstamp_mode = 0;
    s->appl_ptr = 0;
    s->hw_ptr = 0;
    s->hw_frames = 0;
    s->avail_max = 0;
    s->configured = 1;
    s->state = SNDRV_PCM_STATE_SETUP;
    ring_clear(s);
    return 0;
}

static int pcm_hw_free(struct snd_stream *s) {
    if (s->state != SNDRV_PCM_STATE_SETUP && s->state != SNDRV_PCM_STATE_PREPARED &&
        s->state != SNDRV_PCM_STATE_OPEN && s->state != SNDRV_PCM_STATE_XRUN)
        return -EBADFD;
    stream_stop(s, SNDRV_PCM_STATE_OPEN);
    if (s->configured && s->ops->release) s->ops->release(s->card->backend->context);
    s->configured = 0;
    return 0;
}

static int pcm_sw_params(struct snd_stream *s, struct snd_pcm_sw_params *params) {
    if (!s->configured || s->state == SNDRV_PCM_STATE_OPEN) return -EBADFD;
    if (params->tstamp_mode > 1) return -EINVAL;
    if (params->proto >= SNDRV_PROTOCOL_VERSION(2, 0, 12) && params->tstamp_type > 2U)
        return -EINVAL;
    if (!params->avail_min) return -EINVAL;
    if (params->silence_size >= s->boundary) {
        if (params->silence_threshold != 0) return -EINVAL;
    } else {
        if (params->silence_size > params->silence_threshold) return -EINVAL;
        if (params->silence_threshold > s->buffer_size) return -EINVAL;
    }
    s->tstamp_mode = params->tstamp_mode;
    if (params->proto >= SNDRV_PROTOCOL_VERSION(2, 0, 12))
        s->tstamp_type = (int)params->tstamp_type;
    s->avail_min = params->avail_min;
    s->wake_need = params->avail_min;
    s->start_threshold = params->start_threshold;
    s->stop_threshold = params->stop_threshold;
    s->silence_threshold = params->silence_threshold;
    s->silence_size = params->silence_size;
    s->silence_filled = 0;
    params->boundary = s->boundary;
    if (s->direction == SND_PLAYBACK && s->silence_size >= s->boundary &&
        s->state == SNDRV_PCM_STATE_PREPARED) {
        int64_t free_frames = playback_avail(s);
        if (free_frames > 0) ring_zero_frames(s, s->appl_ptr, (uint64_t)free_frames);
    }
    stream_update(s);
    stream_wake(s);
    return 0;
}

static int stream_check_io_state(const struct snd_stream *s) {
    switch (s->state) {
    case SNDRV_PCM_STATE_PREPARED:
    case SNDRV_PCM_STATE_RUNNING:
    case SNDRV_PCM_STATE_PAUSED:       return 0;
    case SNDRV_PCM_STATE_DRAINING:     return s->direction == SND_CAPTURE ? 0 : -EBADFD;
    case SNDRV_PCM_STATE_XRUN:         return -EPIPE;
    case SNDRV_PCM_STATE_SUSPENDED:    return -ESTRPIPE;
    case SNDRV_PCM_STATE_DISCONNECTED: return -ENODEV;
    default:                           return -EBADFD;
    }
}

static int stream_wait(struct snd_card *card, struct snd_stream *s, uint64_t need) {
    unsigned generation = s->generation;
    uint64_t deadline = time_uptime_ns() + WAIT_TIMEOUT_NS;
    for (;;) {
        int idle = s->state == SNDRV_PCM_STATE_PAUSED || s->state == SNDRV_PCM_STATE_PREPARED;
        s->waiters++;
        s->wake_need = need < s->avail_min ? need : s->avail_min;
        if (!s->wake_need) s->wake_need = 1;
        lock_release(&card->lock);
        int status = process_prepare_wait_interruptible(s, idle ? 0 : deadline);
        lock_acquire(&card->lock);
        if (status != 0) {
            s->waiters--;
            s->wake_need = s->avail_min;
            return -ERESTARTSYS;
        }
        stream_update(s);
        int ready = s->generation != generation || stream_check_io_state(s) != 0 ||
            stream_avail(s) >= (int64_t)s->wake_need ||
            (s->direction == SND_CAPTURE && s->state == SNDRV_PCM_STATE_DRAINING);
        lock_release(&card->lock);
        if (!ready) process_wait();
        process_finish_wait();
        lock_acquire(&card->lock);
        s->waiters--;
        s->wake_need = s->avail_min;
        stream_update(s);
        if (s->generation != generation) return -EBADFD;
        int state = stream_check_io_state(s);
        if (state != 0) return state;
        if (stream_avail(s) > 0) return 0;
        if (s->direction == SND_CAPTURE && s->state == SNDRV_PCM_STATE_DRAINING) return 0;
        if (process_wait_interrupted()) return -ERESTARTSYS;
        if (!idle && time_uptime_ns() >= deadline) {
            kprintf("SOUND: card %d %s stalled, no DMA progress\n", card->index,
                    s->direction == SND_PLAYBACK ? "playback" : "capture");
            return -EIO;
        }
    }
}

static int64_t stream_transfer(struct snd_card *card, struct snd_stream *s, struct file *file,
                               uint64_t user, uint64_t frames, int kernel) {
    if (!s->configured) return -EBADFD;
    int state = stream_check_io_state(s);
    if (state != 0) return state;
    if (!frames) return 0;
    int nonblock = (file->flags & O_NONBLOCK_FLAG) != 0;
    if (s->direction == SND_CAPTURE && s->state == SNDRV_PCM_STATE_PREPARED &&
        frames >= s->start_threshold) {
        int started = stream_start(s);
        if (started < 0) return started;
    }
    uint64_t done = 0;
    int64_t error = 0;
    while (frames) {
        stream_update(s);
        state = stream_check_io_state(s);
        if (state != 0) {
            error = state;
            break;
        }
        int64_t avail = stream_avail(s);
        if (s->direction == SND_CAPTURE && s->state == SNDRV_PCM_STATE_DRAINING && avail <= 0) {
            s->state = SNDRV_PCM_STATE_SETUP;
            error = -EBADFD;
            break;
        }
        if (avail <= 0) {
            if (nonblock) {
                error = -EAGAIN;
                break;
            }
            int waited = stream_wait(card, s, frames);
            if (waited < 0) {
                error = waited;
                break;
            }
            continue;
        }
        uint64_t count = frames < (uint64_t)avail ? frames : (uint64_t)avail;
        uint64_t offset = s->appl_ptr % s->buffer_size;
        if (count > s->buffer_size - offset) count = s->buffer_size - offset;
        unsigned generation = s->generation;
        uint64_t appl = s->appl_ptr;
        lock_release(&card->lock);
        int copied = ring_copy_user(s, offset * s->frame_bytes, user, count * s->frame_bytes,
                                    s->direction == SND_PLAYBACK, kernel);
        lock_acquire(&card->lock);
        if (copied != 0) {
            error = -EFAULT;
            break;
        }
        if (s->generation != generation || s->appl_ptr != appl) {
            error = stream_check_io_state(s);
            if (!error) error = -EBADFD;
            break;
        }
        s->appl_ptr = (s->appl_ptr + count) % s->boundary;
        done += count;
        frames -= count;
        user += count * s->frame_bytes;
        if (s->direction == SND_PLAYBACK && s->state == SNDRV_PCM_STATE_PREPARED &&
            stream_hw_avail(s) >= (int64_t)s->start_threshold) {
            int started = stream_start(s);
            if (started < 0) {
                error = started;
                break;
            }
        }
    }
    if (done) return (int64_t)done;
    return error;
}

static int stream_drain(struct snd_card *card, struct snd_stream *s, struct file *file) {
    if (!s->configured || s->state == SNDRV_PCM_STATE_OPEN) return -EBADFD;
    if (s->state == SNDRV_PCM_STATE_SUSPENDED) return -ESTRPIPE;
    if (s->state == SNDRV_PCM_STATE_DISCONNECTED) return -ENODEV;
    if (s->state == SNDRV_PCM_STATE_PAUSED) {
        stream_trigger(s, SND_TRIGGER_RELEASE);
        s->state = SNDRV_PCM_STATE_RUNNING;
        s->update_ns = time_uptime_ns();
    }
    if (s->direction == SND_CAPTURE) {
        if (s->state == SNDRV_PCM_STATE_RUNNING) {
            stream_update(s);
            stream_trigger(s, SND_TRIGGER_STOP);
            s->state = capture_avail(s) > 0 ? SNDRV_PCM_STATE_DRAINING : SNDRV_PCM_STATE_SETUP;
            s->generation++;
            stream_wake(s);
        } else if (s->state != SNDRV_PCM_STATE_DRAINING) {
            stream_stop(s, SNDRV_PCM_STATE_SETUP);
        }
        return 0;
    }
    if (s->state == SNDRV_PCM_STATE_PREPARED) {
        if (stream_hw_avail(s) <= 0) {
            stream_stop(s, SNDRV_PCM_STATE_SETUP);
            return 0;
        }
        int started = stream_start(s);
        if (started < 0) return started;
    }
    if (s->state == SNDRV_PCM_STATE_XRUN || s->state == SNDRV_PCM_STATE_SETUP) {
        stream_stop(s, SNDRV_PCM_STATE_SETUP);
        return 0;
    }
    if (s->state == SNDRV_PCM_STATE_RUNNING) s->state = SNDRV_PCM_STATE_DRAINING;
    if (s->state != SNDRV_PCM_STATE_DRAINING) return 0;
    if (file->flags & O_NONBLOCK_FLAG) return -EAGAIN;

    uint64_t remaining = stream_hw_avail(s) > 0 ? (uint64_t)stream_hw_avail(s) : 0;
    uint64_t deadline =
        time_uptime_ns() + remaining * NSEC / (s->rate ? s->rate : 1U) + WAIT_TIMEOUT_NS;
    while (s->state == SNDRV_PCM_STATE_DRAINING) {
        s->waiters++;
        s->wake_need = s->buffer_size;
        lock_release(&card->lock);
        int status = process_prepare_wait_interruptible(s, deadline);
        lock_acquire(&card->lock);
        if (status != 0) {
            s->waiters--;
            s->wake_need = s->avail_min;
            return -ERESTARTSYS;
        }
        stream_update(s);
        int ready = s->state != SNDRV_PCM_STATE_DRAINING;
        lock_release(&card->lock);
        if (!ready) process_wait();
        process_finish_wait();
        lock_acquire(&card->lock);
        s->waiters--;
        s->wake_need = s->avail_min;
        stream_update(s);
        if (s->state != SNDRV_PCM_STATE_DRAINING) break;
        if (process_wait_interrupted()) return -ERESTARTSYS;
        if (time_uptime_ns() >= deadline) {
            stream_stop(s, SNDRV_PCM_STATE_SETUP);
            return -EIO;
        }
    }
    return 0;
}

static int stream_pause(struct snd_stream *s, int push) {
    if (!s->configured) return -EBADFD;
    if (push) {
        if (s->state != SNDRV_PCM_STATE_RUNNING) return -EBADFD;
        stream_update(s);
        if (s->state != SNDRV_PCM_STATE_RUNNING)
            return s->state == SNDRV_PCM_STATE_XRUN ? -EPIPE : -EBADFD;
        if (s->ops->trigger(s->card->backend->context, SND_TRIGGER_PAUSE) != 0) return -EIO;
        s->state = SNDRV_PCM_STATE_PAUSED;
    } else {
        if (s->state != SNDRV_PCM_STATE_PAUSED) return -EBADFD;
        if (s->ops->trigger(s->card->backend->context, SND_TRIGGER_RELEASE) != 0) return -EIO;
        s->state = SNDRV_PCM_STATE_RUNNING;
        s->update_ns = time_uptime_ns();
    }
    s->trigger_ns = stream_now(s);
    stream_wake(s);
    return 0;
}

static int stream_sync_state(struct snd_stream *s) {
    switch (s->state) {
    case SNDRV_PCM_STATE_PREPARED:
    case SNDRV_PCM_STATE_RUNNING:
    case SNDRV_PCM_STATE_DRAINING:
    case SNDRV_PCM_STATE_PAUSED:       return 0;
    case SNDRV_PCM_STATE_XRUN:         return -EPIPE;
    case SNDRV_PCM_STATE_SUSPENDED:    return -ESTRPIPE;
    case SNDRV_PCM_STATE_DISCONNECTED: return -ENODEV;
    default:                           return -EBADFD;
    }
}

static int64_t stream_move(struct snd_stream *s, uint64_t frames, int forward) {
    int status = stream_sync_state(s);
    if (status != 0) return status;
    stream_update(s);
    status = stream_sync_state(s);
    if (status != 0) return status;
    int64_t limit = forward ? stream_avail(s) : stream_hw_avail(s);
    if (limit < 0) limit = 0;
    if (frames > (uint64_t)limit) frames = (uint64_t)limit;
    if (forward) {
        s->appl_ptr = (s->appl_ptr + frames) % s->boundary;
    } else {
        s->appl_ptr = (s->appl_ptr + s->boundary - frames) % s->boundary;
    }
    s->silence_filled = 0;
    return (int64_t)frames;
}

static void fill_status(struct snd_stream *s, struct snd_pcm_status *status) {
    memset(status, 0, sizeof(*status));
    if (s->configured) stream_update(s);
    status->state = s->state;
    ns_to_timespec(s->trigger_ns, &status->trigger_tstamp);
    uint64_t when = s->update_ns ? s->update_ns : time_uptime_ns();
    if (s->tstamp_type == SNDRV_PCM_TSTAMP_TYPE_GETTIMEOFDAY)
        when = time_realtime_ns() - (time_uptime_ns() - when);
    ns_to_timespec(when, &status->tstamp);
    status->appl_ptr = s->appl_ptr;
    status->hw_ptr = s->hw_ptr;
    if (s->configured) {
        int64_t avail = stream_avail(s);
        status->avail = avail > 0 ? (unsigned long)avail : 0;
        if (s->direction == SND_PLAYBACK) {
            int64_t delay = stream_hw_avail(s);
            if (stream_running(s) || s->state == SNDRV_PCM_STATE_PAUSED)
                delay += s->ops->hardware.fifo_frames;
            status->delay = (long)delay;
        } else {
            status->delay = (long)avail;
        }
        if (s->rate)
            ns_to_timespec(s->hw_frames / s->rate * NSEC + s->hw_frames % s->rate * NSEC / s->rate,
                           &status->audio_tstamp);
    }
    status->avail_max = s->avail_max;
    s->avail_max = 0;
}

static int64_t ioctl_sync_ptr(struct snd_card *card, struct snd_stream *s, uint64_t argument) {
    struct snd_pcm_sync_ptr sync;
    lock_release(&card->lock);
    int copied = copy_from_user(&sync, argument, sizeof(sync));
    lock_acquire(&card->lock);
    if (copied != 0) return -EFAULT;
    if (!s->configured) sync.flags |= SNDRV_PCM_SYNC_PTR_APPL;
    if ((sync.flags & SNDRV_PCM_SYNC_PTR_HWSYNC) && s->configured) {
        int status = stream_sync_state(s);
        if (status != 0 && status != -EPIPE) return status;
        stream_update(s);
    }
    if (!(sync.flags & SNDRV_PCM_SYNC_PTR_APPL)) {
        if (sync.c.control.appl_ptr >= s->boundary) return -EINVAL;
        if (sync.c.control.appl_ptr != s->appl_ptr) {
            s->appl_ptr = sync.c.control.appl_ptr;
            s->silence_filled = 0;
            if (stream_running(s)) stream_update(s);
        }
    }
    if (!(sync.flags & SNDRV_PCM_SYNC_PTR_AVAIL_MIN)) {
        if (!sync.c.control.avail_min) sync.c.control.avail_min = 1;
        s->avail_min = sync.c.control.avail_min;
        s->wake_need = s->avail_min;
    }
    memset(&sync.s, 0, sizeof(sync.s));
    memset(&sync.c, 0, sizeof(sync.c));
    sync.s.status.state = s->state;
    sync.s.status.hw_ptr = s->hw_ptr;
    ns_to_timespec(s->update_ns ? s->update_ns : time_uptime_ns(), &sync.s.status.tstamp);
    if (s->rate)
        ns_to_timespec(s->hw_frames / s->rate * NSEC + s->hw_frames % s->rate * NSEC / s->rate,
                       &sync.s.status.audio_tstamp);
    sync.c.control.appl_ptr = s->appl_ptr;
    sync.c.control.avail_min = s->avail_min;
    lock_release(&card->lock);
    copied = copy_to_user(argument, &sync, sizeof(sync));
    lock_acquire(&card->lock);
    return copied == 0 ? 0 : -EFAULT;
}

static void fill_pcm_info(struct snd_card *card, int direction, struct snd_pcm_info *info) {
    memset(info, 0, sizeof(*info));
    const struct snd_backend *backend = card->backend;
    info->device = 0;
    info->subdevice = 0;
    info->stream = direction;
    info->card = card->index;
    copy_field(info->id, sizeof(info->id), backend->driver);
    copy_field(info->name, sizeof(info->name), backend->name);
    copy_field(info->subname, sizeof(info->subname), "subdevice #0");
    info->subdevices_count = 1;
    info->subdevices_avail = card->streams[direction].open ? 0U : 1U;
}

struct pcm_open_file {
    struct snd_card *card;
    struct snd_stream *stream;
};

static struct snd_stream *pcm_file_stream(struct file *file, struct snd_card **card) {
    struct pcm_open_file *open_file = file->private_data;
    if (!open_file) return NULL;
    *card = open_file->card;
    return open_file->stream;
}

static int pcm_open(struct file *file) {
    int direction = 0;
    lock_acquire(&cards_lock);
    struct snd_card *card = card_of_minor(file->node->dev_minor, &direction);
    if (!card || !card->backend->streams[direction]) {
        lock_release(&cards_lock);
        return -ENODEV;
    }
    struct pcm_open_file *open_file = kmalloc(sizeof(*open_file));
    if (!open_file) {
        lock_release(&cards_lock);
        return -ENOMEM;
    }
    lock_acquire(&card->lock);
    struct snd_stream *s = &card->streams[direction];
    if (s->open) {
        lock_release(&card->lock);
        lock_release(&cards_lock);
        kfree(open_file);
        return -EBUSY;
    }
    s->open = 1;
    s->state = SNDRV_PCM_STATE_OPEN;
    s->configured = 0;
    s->tstamp_type = SNDRV_PCM_TSTAMP_TYPE_GETTIMEOFDAY;
    s->xruns = 0;
    lock_release(&card->lock);
    if (card->owner) (void)module_get(card->owner);
    lock_release(&cards_lock);
    open_file->card = card;
    open_file->stream = s;
    file->private_data = open_file;
    return 0;
}

static void pcm_release(struct file *file) {
    struct snd_card *card = NULL;
    struct snd_stream *s = pcm_file_stream(file, &card);
    if (!s) return;
    lock_acquire(&card->lock);
    if (card->present) {
        stream_stop(s, SNDRV_PCM_STATE_OPEN);
        if (s->configured && s->ops && s->ops->release) s->ops->release(card->backend->context);
    }
    s->configured = 0;
    s->open = 0;
    struct module *owner = card->owner;
    lock_release(&card->lock);
    if (owner) module_put(owner);
    kfree(file->private_data);
    file->private_data = NULL;
}

static int64_t pcm_rw(struct file *file, uint64_t user, size_t size, int writing) {
    struct snd_card *card = NULL;
    struct snd_stream *s = pcm_file_stream(file, &card);
    if (!s) return -EBADFD;
    lock_acquire(&card->lock);
    int64_t result = -EBADFD;
    if (card->present && s->configured && s->frame_bytes &&
        (s->direction == SND_PLAYBACK) == (writing != 0)) {
        uint64_t frames = size / s->frame_bytes;
        result = frames ? stream_transfer(card, s, file, user, frames, 1) : -EINVAL;
        if (result > 0) result *= s->frame_bytes;
    }
    lock_release(&card->lock);
    return result;
}

static uint32_t pcm_poll(struct file *file) {
    struct snd_card *card = NULL;
    struct snd_stream *s = pcm_file_stream(file, &card);
    if (!s) return VFS_POLL_ERR;
    lock_acquire(&card->lock);
    uint32_t ok = s->direction == SND_PLAYBACK ? VFS_POLL_OUT : VFS_POLL_IN;
    uint32_t mask = 0;
    if (!card->present) {
        mask = ok | VFS_POLL_ERR | VFS_POLL_HUP;
    } else {
        if (s->configured) stream_update(s);
        int64_t avail = s->configured ? stream_avail(s) : 0;
        switch (s->state) {
        case SNDRV_PCM_STATE_RUNNING:
        case SNDRV_PCM_STATE_PREPARED:
        case SNDRV_PCM_STATE_PAUSED:
            if (avail >= (int64_t)s->avail_min) mask = ok;
            break;
        case SNDRV_PCM_STATE_DRAINING:
            if (s->direction == SND_CAPTURE) {
                mask = ok;
                if (avail <= 0) mask |= VFS_POLL_ERR;
            }
            break;
        default: mask = ok | VFS_POLL_ERR; break;
        }
    }
    lock_release(&card->lock);
    return mask;
}

static int64_t copy_out(struct snd_card *card, uint64_t user, const void *source, size_t size) {
    lock_release(&card->lock);
    int status = copy_to_user(user, source, size);
    lock_acquire(&card->lock);
    return status == 0 ? 0 : -EFAULT;
}

static int64_t copy_in(struct snd_card *card, void *destination, uint64_t user, size_t size) {
    lock_release(&card->lock);
    int status = copy_from_user(destination, user, size);
    lock_acquire(&card->lock);
    return status == 0 ? 0 : -EFAULT;
}

static int64_t pcm_ioctl_locked(struct snd_card *card, struct snd_stream *s, struct file *file,
                                unsigned long request, uint64_t argument) {
    if (SND_IOC_TYPE(request) != 'A') return -ENOTTY;
    switch (SND_IOC_NR(request)) {
    case 0x00: {
        int version = SNDRV_PCM_VERSION;
        return copy_out(card, argument, &version, sizeof(version));
    }
    case 0x01: {
        struct snd_pcm_info info;
        fill_pcm_info(card, s->direction, &info);
        return copy_out(card, argument, &info, sizeof(info));
    }
    case 0x02: return 0;
    case 0x03: {
        int type = 0;
        if (copy_in(card, &type, argument, sizeof(type)) != 0) return -EFAULT;
        if (type < 0 || type > SNDRV_PCM_TSTAMP_TYPE_MONOTONIC_RAW) return -EINVAL;
        s->tstamp_type = type;
        return 0;
    }
    case 0x04:                         return 0;
    case SNDRV_PCM_IOCTL_NR_HW_REFINE:
    case SNDRV_PCM_IOCTL_NR_HW_PARAMS: {
        struct snd_pcm_hw_params params;
        if (copy_in(card, &params, argument, sizeof(params)) != 0) return -EFAULT;
        int status = SND_IOC_NR(request) == SNDRV_PCM_IOCTL_NR_HW_REFINE
            ? pcm_hw_refine(s, &params)
            : pcm_hw_params(s, &params);
        if (status != 0) return status;
        return copy_out(card, argument, &params, sizeof(params));
    }
    case SNDRV_PCM_IOCTL_NR_HW_FREE:   return pcm_hw_free(s);
    case SNDRV_PCM_IOCTL_NR_SW_PARAMS: {
        struct snd_pcm_sw_params params;
        if (copy_in(card, &params, argument, sizeof(params)) != 0) return -EFAULT;
        int status = pcm_sw_params(s, &params);
        if (status != 0) return status;
        return copy_out(card, argument, &params, sizeof(params));
    }
    case SNDRV_PCM_IOCTL_NR_STATUS:
    case SNDRV_PCM_IOCTL_NR_STATUS_EXT: {
        struct snd_pcm_status status;
        fill_status(s, &status);
        return copy_out(card, argument, &status, sizeof(status));
    }
    case SNDRV_PCM_IOCTL_NR_DELAY: {
        int state = stream_sync_state(s);
        if (state != 0) return state;
        stream_update(s);
        state = stream_sync_state(s);
        if (state != 0) return state;
        long delay =
            s->direction == SND_PLAYBACK ? (long)stream_hw_avail(s) : (long)stream_avail(s);
        if (s->direction == SND_PLAYBACK && stream_running(s))
            delay += (long)s->ops->hardware.fifo_frames;
        return copy_out(card, argument, &delay, sizeof(delay));
    }
    case SNDRV_PCM_IOCTL_NR_HWSYNC: {
        int state = stream_sync_state(s);
        if (state != 0) return state;
        stream_update(s);
        return stream_sync_state(s);
    }
    case SNDRV_PCM_IOCTL_NR_SYNC_PTR:     return ioctl_sync_ptr(card, s, argument);
    case SNDRV_PCM_IOCTL_NR_CHANNEL_INFO: {
        struct snd_pcm_channel_info info;
        if (copy_in(card, &info, argument, sizeof(info)) != 0) return -EFAULT;
        if (!s->configured) return -EBADFD;
        if (info.channel >= s->channels) return -EINVAL;
        info.offset = 0;
        info.first = info.channel * s->sample_bits;
        info.step = s->frame_bits;
        return copy_out(card, argument, &info, sizeof(info));
    }
    case SNDRV_PCM_IOCTL_NR_PREPARE: return stream_prepare(s);
    case SNDRV_PCM_IOCTL_NR_RESET:   {
        int state = stream_sync_state(s);
        if (state != 0 && state != -EPIPE) return state;
        if (s->configured) stream_update(s);
        s->appl_ptr = s->hw_ptr;
        s->silence_filled = 0;
        s->generation++;
        stream_wake(s);
        return 0;
    }
    case SNDRV_PCM_IOCTL_NR_START: {
        if (!s->configured) return -EBADFD;
        if (s->state == SNDRV_PCM_STATE_RUNNING) return -EBADFD;
        return stream_start(s);
    }
    case SNDRV_PCM_IOCTL_NR_DROP:
        if (!s->configured || s->state == SNDRV_PCM_STATE_OPEN) return -EBADFD;
        stream_stop(s, SNDRV_PCM_STATE_SETUP);
        return 0;
    case SNDRV_PCM_IOCTL_NR_DRAIN:  return stream_drain(card, s, file);
    case SNDRV_PCM_IOCTL_NR_PAUSE:  return stream_pause(s, argument != 0);
    case SNDRV_PCM_IOCTL_NR_RESUME: return -ENOSYS;
    case SNDRV_PCM_IOCTL_NR_XRUN:
        if (!s->configured) return -EBADFD;
        if (s->state == SNDRV_PCM_STATE_XRUN) return 0;
        if (!stream_running(s) && s->state != SNDRV_PCM_STATE_PAUSED &&
            s->state != SNDRV_PCM_STATE_PREPARED)
            return -EBADFD;
        stream_set_xrun(s);
        stream_wake(s);
        return 0;
    case SNDRV_PCM_IOCTL_NR_REWIND:
    case SNDRV_PCM_IOCTL_NR_FORWARD: {
        unsigned long frames = 0;
        if (copy_in(card, &frames, argument, sizeof(frames)) != 0) return -EFAULT;
        int64_t moved = stream_move(s, frames, SND_IOC_NR(request) == SNDRV_PCM_IOCTL_NR_FORWARD);
        long reply = (long)moved;
        if (copy_out(card, argument, &reply, sizeof(reply)) != 0) return -EFAULT;
        return moved < 0 ? moved : 0;
    }
    case SNDRV_PCM_IOCTL_NR_WRITEI_FRAMES:
    case SNDRV_PCM_IOCTL_NR_READI_FRAMES:  {
        int writing = SND_IOC_NR(request) == SNDRV_PCM_IOCTL_NR_WRITEI_FRAMES;
        if ((s->direction == SND_PLAYBACK) != writing) return -EINVAL;
        struct snd_xferi transfer;
        if (copy_in(card, &transfer, argument, sizeof(transfer)) != 0) return -EFAULT;
        if (!s->configured) return -EBADFD;
        if (s->access != SNDRV_PCM_ACCESS_RW_INTERLEAVED &&
            s->access != SNDRV_PCM_ACCESS_MMAP_INTERLEAVED)
            return -EINVAL;
        int64_t result = stream_transfer(card, s, file, transfer.buf, transfer.frames, 0);
        transfer.result = (long)result;
        if (copy_out(card, argument, &transfer, sizeof(transfer)) != 0) return -EFAULT;
        return result < 0 ? result : 0;
    }
    case SNDRV_PCM_IOCTL_NR_WRITEN_FRAMES:
    case SNDRV_PCM_IOCTL_NR_READN_FRAMES:  return -EINVAL;
    case SNDRV_PCM_IOCTL_NR_LINK:
    case SNDRV_PCM_IOCTL_NR_UNLINK:        return -EINVAL;
    default:                               return -ENOTTY;
    }
}

#define SOUND_REFUSALS_REPORTED 16U

static int64_t pcm_ioctl(struct file *file, unsigned long request, uint64_t argument) {
    struct snd_card *card = NULL;
    struct snd_stream *s = pcm_file_stream(file, &card);
    if (!s) return -EBADFD;
    lock_acquire(&card->lock);
    int64_t result = card->present ? pcm_ioctl_locked(card, s, file, request, argument) : -ENODEV;
    int state = s->state;
    lock_release(&card->lock);
    static unsigned reported;
    if (result < 0 && result != -EAGAIN && result != -EPIPE && result != -ERESTARTSYS &&
        result != -ENOTTY && reported < SOUND_REFUSALS_REPORTED) {
        reported++;
        kprintf("SOUND: card %d pcm request %x refused (%d), state %d\n", card->index,
                (unsigned)SND_IOC_NR(request), (int)result, state);
    }
    return result;
}

static int64_t pcm_write_file(struct file *file, size_t size, const void *buffer) {
    return pcm_rw(file, (uint64_t)(uintptr_t)buffer, size, 1);
}

static int64_t pcm_read_file(struct file *file, size_t size, void *buffer) {
    return pcm_rw(file, (uint64_t)(uintptr_t)buffer, size, 0);
}

static const struct vfs_file_ops pcm_file_ops = {
    .open = pcm_open,
    .release = pcm_release,
    .read = pcm_read_file,
    .write = pcm_write_file,
    .poll = pcm_poll,
    .ioctl = pcm_ioctl,
};

const struct vfs_file_ops *sound_pcm_file_ops(void) { return &pcm_file_ops; }

int64_t sound_pcm_mmap(struct vfs_node *node, struct file *file, uint64_t cr3,
                       uint64_t virtual_address, uint64_t length, uint64_t offset,
                       uint64_t page_flags) {
    (void)node;
    struct snd_card *card = NULL;
    struct snd_stream *s = pcm_file_stream(file, &card);
    if (!s || !length) return -EINVAL;
    if (offset != SNDRV_PCM_MMAP_OFFSET_DATA) return -ENXIO;
    lock_acquire(&card->lock);
    if (!card->present || !s->configured) {
        lock_release(&card->lock);
        return -EBADFD;
    }
    uint64_t pages = (length + 4095U) / 4096U;
    if (pages > s->ring_pages) {
        lock_release(&card->lock);
        return -EINVAL;
    }
    uint64_t flags = page_flags | PAGE_USER | PAGE_PRESENT | PAGE_NX | PAGE_SHARED;
    uint64_t mapped = 0;
    int64_t result = 0;
    for (; mapped < pages; mapped++) {
        uint64_t physical = s->ring_physical[mapped];
        if (pmm_page_ref(physical) != 0) {
            result = -ENOMEM;
            break;
        }
        if (vmm_map_page_in(cr3, virtual_address + mapped * 4096U, physical, flags) != 0) {
            pmm_free_page((void *)physical);
            result = -ENOMEM;
            break;
        }
    }
    if (result != 0) {
        while (mapped) {
            mapped--;
            (void)vmm_unmap_page_in(cr3, virtual_address + mapped * 4096U);
            pmm_free_page((void *)s->ring_physical[mapped]);
        }
    }
    lock_release(&card->lock);
    return result;
}

static unsigned control_count(const struct snd_card *card) {
    unsigned count = card->backend->control_count;
    return count > CONTROL_MAX ? CONTROL_MAX : count;
}

static void fill_element_id(const struct snd_card *card, unsigned index,
                            struct snd_ctl_elem_id *id) {
    const struct snd_control *control = &card->backend->controls[index];
    memset(id, 0, sizeof(*id));
    id->numid = index + 1U;
    id->iface = (int)control->iface;
    id->index = control->index;
    copy_field(id->name, sizeof(id->name), control->name);
}

static int element_lookup(const struct snd_card *card, const struct snd_ctl_elem_id *id) {
    unsigned count = control_count(card);
    if (id->numid) return id->numid <= count ? (int)id->numid - 1 : -ENOENT;
    for (unsigned index = 0; index < count; index++) {
        const struct snd_control *control = &card->backend->controls[index];
        if ((int)control->iface != id->iface || control->index != id->index) continue;
        if (id->device || id->subdevice) continue;
        if (!strncmp((const char *)id->name, control->name, sizeof(id->name))) return (int)index;
    }
    return -ENOENT;
}

static void ctl_queue_event(struct snd_card *card, unsigned index, unsigned mask) {
    if (!card->subscribers) return;
    struct snd_ctl_event event;
    memset(&event, 0, sizeof(event));
    event.type = SNDRV_CTL_EVENT_ELEM;
    event.data.elem.mask = mask;
    fill_element_id(card, index, &event.data.elem.id);
    for (struct snd_ctl_subscriber *sub = card->subscribers; sub; sub = sub->next) {
        if (!sub->subscribed) continue;
        int merged = 0;
        for (unsigned at = 0; at < sub->count; at++) {
            struct snd_ctl_event *queued = &sub->events[(sub->head + at) % EVENT_QUEUE];
            if (queued->data.elem.id.numid == event.data.elem.id.numid) {
                queued->data.elem.mask |= mask;
                merged = 1;
                break;
            }
        }
        if (merged) continue;
        if (sub->count == EVENT_QUEUE) {
            sub->head = (sub->head + 1U) % EVENT_QUEUE;
            sub->count--;
        }
        sub->events[(sub->head + sub->count) % EVENT_QUEUE] = event;
        sub->count++;
    }
    process_wake_all(card);
}

static int64_t ioctl_elem_list(struct snd_card *card, uint64_t argument) {
    struct snd_ctl_elem_list list;
    if (copy_in(card, &list, argument, sizeof(list)) != 0) return -EFAULT;
    unsigned total = control_count(card);
    list.count = total;
    list.used = 0;
    for (unsigned index = list.offset; index < total && list.used < list.space; index++) {
        struct snd_ctl_elem_id id;
        fill_element_id(card, index, &id);
        if (copy_out(card, list.pids + (uint64_t)list.used * sizeof(id), &id, sizeof(id)) != 0)
            return -EFAULT;
        list.used++;
    }
    return copy_out(card, argument, &list, sizeof(list));
}

static int64_t ioctl_elem_info(struct snd_card *card, uint64_t argument) {
    struct snd_ctl_elem_info info;
    if (copy_in(card, &info, argument, sizeof(info)) != 0) return -EFAULT;
    int index = element_lookup(card, &info.id);
    if (index < 0) return index;
    const struct snd_control *control = &card->backend->controls[index];
    unsigned requested_item = info.value.enumerated.item;
    memset(&info.value, 0, sizeof(info.value));
    memset(info.dimen, 0, sizeof(info.dimen));
    fill_element_id(card, (unsigned)index, &info.id);
    info.type = (int)control->type;
    info.access = control->access & (SND_CONTROL_READ | SND_CONTROL_WRITE | SND_CONTROL_VOLATILE);
    if (control->db_step || control->db_min) info.access |= SNDRV_CTL_ELEM_ACCESS_TLV_READ;
    info.count = control->count ? control->count : 1U;
    info.owner = -1;
    if (control->type == SND_CONTROL_ENUMERATED) {
        info.value.enumerated.items = control->item_count;
        if (control->item_count) {
            if (requested_item >= control->item_count) requested_item = control->item_count - 1U;
            info.value.enumerated.item = requested_item;
            copy_field((unsigned char *)info.value.enumerated.name,
                       sizeof(info.value.enumerated.name), control->items[requested_item]);
        }
    } else if (control->type == SND_CONTROL_BOOLEAN) {
        info.value.integer.min = 0;
        info.value.integer.max = 1;
        info.value.integer.step = 0;
    } else {
        info.value.integer.min = control->min;
        info.value.integer.max = control->max;
        info.value.integer.step = control->step;
    }
    return copy_out(card, argument, &info, sizeof(info));
}

static int64_t ioctl_elem_read(struct snd_card *card, uint64_t argument) {
    struct snd_ctl_elem_value value;
    if (copy_in(card, &value, argument, sizeof(value)) != 0) return -EFAULT;
    int index = element_lookup(card, &value.id);
    if (index < 0) return index;
    const struct snd_control *control = &card->backend->controls[index];
    if (!(control->access & SND_CONTROL_READ)) return -EPERM;
    long values[8];
    memset(values, 0, sizeof(values));
    if (card->backend->control_get(card->backend->context, (unsigned)index, values) != 0)
        return -EIO;
    fill_element_id(card, (unsigned)index, &value.id);
    memset(&value.value, 0, sizeof(value.value));
    unsigned count = control->count ? control->count : 1U;
    if (count > 8U) count = 8U;
    for (unsigned at = 0; at < count; at++) {
        if (control->type == SND_CONTROL_ENUMERATED)
            value.value.enumerated_item[at] = (unsigned)values[at];
        else value.value.integer_value[at] = values[at];
    }
    return copy_out(card, argument, &value, sizeof(value));
}

static int64_t ioctl_elem_write(struct snd_card *card, uint64_t argument) {
    struct snd_ctl_elem_value value;
    if (copy_in(card, &value, argument, sizeof(value)) != 0) return -EFAULT;
    int index = element_lookup(card, &value.id);
    if (index < 0) return index;
    const struct snd_control *control = &card->backend->controls[index];
    if (!(control->access & SND_CONTROL_WRITE) || !card->backend->control_put) return -EPERM;
    unsigned count = control->count ? control->count : 1U;
    if (count > 8U) count = 8U;
    long values[8];
    memset(values, 0, sizeof(values));
    for (unsigned at = 0; at < count; at++) {
        long item = control->type == SND_CONTROL_ENUMERATED ? (long)value.value.enumerated_item[at]
                                                            : value.value.integer_value[at];
        if (control->type == SND_CONTROL_BOOLEAN && (item < 0 || item > 1)) return -EINVAL;
        if (control->type == SND_CONTROL_INTEGER && (item < control->min || item > control->max))
            return -EINVAL;
        if (control->type == SND_CONTROL_ENUMERATED &&
            (item < 0 || item >= (long)control->item_count))
            return -EINVAL;
        values[at] = item;
    }
    int changed = card->backend->control_put(card->backend->context, (unsigned)index, values);
    if (changed < 0) return changed;
    fill_element_id(card, (unsigned)index, &value.id);
    if (copy_out(card, argument, &value, sizeof(value)) != 0) return -EFAULT;
    if (changed > 0) ctl_queue_event(card, (unsigned)index, SNDRV_CTL_EVENT_MASK_VALUE);
    return 0;
}

static int64_t ioctl_tlv_read(struct snd_card *card, uint64_t argument) {
    struct snd_ctl_tlv header;
    if (copy_in(card, &header, argument, sizeof(header)) != 0) return -EFAULT;
    if (!header.numid || header.numid > control_count(card)) return -ENOENT;
    const struct snd_control *control = &card->backend->controls[header.numid - 1U];
    if (!control->db_step && !control->db_min) return -ENXIO;
    unsigned tlv[4];
    tlv[0] = SNDRV_CTL_TLVT_DB_SCALE;
    tlv[1] = 8;
    tlv[2] = (unsigned)control->db_min;
    tlv[3] = ((unsigned)control->db_step & 0xFFFFU) |
        (control->db_mute ? SNDRV_CTL_TLVD_DB_SCALE_MUTE : 0U);
    if (header.length < sizeof(tlv)) return -ENOMEM;
    return copy_out(card, argument + sizeof(header), tlv, sizeof(tlv));
}

static struct snd_ctl_subscriber *control_subscriber(struct file *file) {
    return file->private_data;
}

static int control_open(struct file *file) {
    lock_acquire(&cards_lock);
    struct snd_card *card = card_of_minor(file->node->dev_minor, NULL);
    if (!card) {
        lock_release(&cards_lock);
        return -ENODEV;
    }
    struct snd_ctl_subscriber *sub = kmalloc(sizeof(*sub));
    if (!sub) {
        lock_release(&cards_lock);
        return -ENOMEM;
    }
    memset(sub, 0, sizeof(*sub));
    sub->card = card;
    lock_acquire(&card->lock);
    sub->next = card->subscribers;
    card->subscribers = sub;
    card->control_opens++;
    lock_release(&card->lock);
    if (card->owner) (void)module_get(card->owner);
    lock_release(&cards_lock);
    file->private_data = sub;
    return 0;
}

static void control_release(struct file *file) {
    struct snd_ctl_subscriber *sub = control_subscriber(file);
    if (!sub) return;
    struct snd_card *card = sub->card;
    lock_acquire(&card->lock);
    for (struct snd_ctl_subscriber **link = &card->subscribers; *link; link = &(*link)->next) {
        if (*link != sub) continue;
        *link = sub->next;
        break;
    }
    card->control_opens--;
    struct module *owner = card->owner;
    lock_release(&card->lock);
    if (owner) module_put(owner);
    kfree(sub);
    file->private_data = NULL;
}

static int64_t control_read(struct file *file, size_t size, void *buffer) {
    struct snd_ctl_subscriber *sub = control_subscriber(file);
    if (!sub) return -EBADFD;
    struct snd_card *card = sub->card;
    if (size < sizeof(struct snd_ctl_event)) return -EINVAL;
    lock_acquire(&card->lock);
    if (!sub->subscribed) {
        lock_release(&card->lock);
        return -EBADFD;
    }
    while (!sub->count) {
        if (!card->present) {
            lock_release(&card->lock);
            return -ENODEV;
        }
        if (file->flags & O_NONBLOCK_FLAG) {
            lock_release(&card->lock);
            return -EAGAIN;
        }
        lock_release(&card->lock);
        int status = process_prepare_wait_interruptible(card, 0);
        if (status != 0) return -ERESTARTSYS;
        lock_acquire(&card->lock);
        int ready = sub->count || !card->present || !sub->subscribed;
        lock_release(&card->lock);
        if (!ready) process_wait();
        process_finish_wait();
        lock_acquire(&card->lock);
        if (!sub->count && process_wait_interrupted()) {
            lock_release(&card->lock);
            return -ERESTARTSYS;
        }
        if (!sub->subscribed) {
            lock_release(&card->lock);
            return -EBADFD;
        }
    }
    size_t done = 0;
    uint8_t *out = buffer;
    while (sub->count && size - done >= sizeof(struct snd_ctl_event)) {
        memcpy(out + done, &sub->events[sub->head], sizeof(struct snd_ctl_event));
        sub->head = (sub->head + 1U) % EVENT_QUEUE;
        sub->count--;
        done += sizeof(struct snd_ctl_event);
    }
    lock_release(&card->lock);
    return (int64_t)done;
}

static int64_t control_write(struct file *file, size_t size, const void *buffer) {
    (void)file;
    (void)size;
    (void)buffer;
    return -EINVAL;
}

static uint32_t control_poll(struct file *file) {
    struct snd_ctl_subscriber *sub = control_subscriber(file);
    if (!sub) return VFS_POLL_ERR;
    struct snd_card *card = sub->card;
    lock_acquire(&card->lock);
    uint32_t mask = 0;
    if (!card->present) mask = VFS_POLL_ERR | VFS_POLL_HUP;
    else if (sub->subscribed && sub->count) mask = VFS_POLL_IN;
    lock_release(&card->lock);
    return mask;
}

static int64_t control_ioctl_locked(struct snd_card *card, struct snd_ctl_subscriber *sub,
                                    unsigned long request, uint64_t argument) {
    if (SND_IOC_TYPE(request) != 'U') return -ENOTTY;
    const struct snd_backend *backend = card->backend;
    switch (SND_IOC_NR(request)) {
    case SNDRV_CTL_IOCTL_NR_PVERSION: {
        int version = SNDRV_CTL_VERSION;
        return copy_out(card, argument, &version, sizeof(version));
    }
    case SNDRV_CTL_IOCTL_NR_CARD_INFO: {
        struct snd_ctl_card_info info;
        memset(&info, 0, sizeof(info));
        info.card = card->index;
        copy_field(info.id, sizeof(info.id), card->id);
        copy_field(info.driver, sizeof(info.driver), backend->driver);
        copy_field(info.name, sizeof(info.name), backend->name);
        copy_field(info.longname, sizeof(info.longname),
                   backend->longname ? backend->longname : backend->name);
        copy_field(info.mixername, sizeof(info.mixername), backend->mixer_name);
        copy_field(info.components, sizeof(info.components), backend->components);
        return copy_out(card, argument, &info, sizeof(info));
    }
    case SNDRV_CTL_IOCTL_NR_ELEM_LIST:   return ioctl_elem_list(card, argument);
    case SNDRV_CTL_IOCTL_NR_ELEM_INFO:   return ioctl_elem_info(card, argument);
    case SNDRV_CTL_IOCTL_NR_ELEM_READ:   return ioctl_elem_read(card, argument);
    case SNDRV_CTL_IOCTL_NR_ELEM_WRITE:  return ioctl_elem_write(card, argument);
    case SNDRV_CTL_IOCTL_NR_ELEM_LOCK:
    case SNDRV_CTL_IOCTL_NR_ELEM_UNLOCK: {
        struct snd_ctl_elem_id id;
        if (copy_in(card, &id, argument, sizeof(id)) != 0) return -EFAULT;
        return element_lookup(card, &id) < 0 ? -ENOENT : 0;
    }
    case SNDRV_CTL_IOCTL_NR_SUBSCRIBE_EVENTS: {
        int value = 0;
        if (copy_in(card, &value, argument, sizeof(value)) != 0) return -EFAULT;
        if (value < 0) {
            value = sub->subscribed;
            return copy_out(card, argument, &value, sizeof(value));
        }
        sub->subscribed = value != 0;
        if (!sub->subscribed) sub->count = 0;
        process_wake_all(card);
        return 0;
    }
    case SNDRV_CTL_IOCTL_NR_TLV_READ:            return ioctl_tlv_read(card, argument);
    case SNDRV_CTL_IOCTL_NR_HWDEP_NEXT_DEVICE:
    case SNDRV_CTL_IOCTL_NR_RAWMIDI_NEXT_DEVICE:
    case SNDRV_CTL_IOCTL_NR_UMP_NEXT_DEVICE:     {
        int device = -1;
        return copy_out(card, argument, &device, sizeof(device));
    }
    case SNDRV_CTL_IOCTL_NR_PCM_NEXT_DEVICE: {
        int device = 0;
        if (copy_in(card, &device, argument, sizeof(device)) != 0) return -EFAULT;
        int any = backend->streams[SND_PLAYBACK] || backend->streams[SND_CAPTURE];
        device = device < 0 && any ? 0 : -1;
        return copy_out(card, argument, &device, sizeof(device));
    }
    case SNDRV_CTL_IOCTL_NR_PCM_INFO: {
        struct snd_pcm_info info;
        if (copy_in(card, &info, argument, sizeof(info)) != 0) return -EFAULT;
        if (info.device != 0 || info.subdevice != 0 || info.stream < 0 || info.stream > 1)
            return -ENXIO;
        if (!backend->streams[info.stream]) return -ENOENT;
        fill_pcm_info(card, info.stream, &info);
        return copy_out(card, argument, &info, sizeof(info));
    }
    case SNDRV_CTL_IOCTL_NR_PCM_PREFER_SUBDEVICE: return 0;
    case SNDRV_CTL_IOCTL_NR_POWER:                return 0;
    case SNDRV_CTL_IOCTL_NR_POWER_STATE:          {
        int state = 0;
        return copy_out(card, argument, &state, sizeof(state));
    }
    default: return -ENOTTY;
    }
}

static int64_t control_ioctl(struct file *file, unsigned long request, uint64_t argument) {
    struct snd_ctl_subscriber *sub = control_subscriber(file);
    if (!sub) return -EBADFD;
    struct snd_card *card = sub->card;
    lock_acquire(&card->lock);
    int64_t result = card->present ? control_ioctl_locked(card, sub, request, argument) : -ENODEV;
    lock_release(&card->lock);
    return result;
}

static const struct vfs_file_ops control_file_ops = {
    .open = control_open,
    .release = control_release,
    .read = control_read,
    .write = control_write,
    .poll = control_poll,
    .ioctl = control_ioctl,
};

const struct vfs_file_ops *sound_control_file_ops(void) { return &control_file_ops; }

static int poller_started;
static const char poller_channel;

static void sound_poller(void *unused) {
    (void)unused;
    for (;;) {
        process_prepare_wait(&poller_channel, time_uptime_ns() + POLL_INTERVAL_NS);
        process_wait();
        process_finish_wait();
        lock_acquire(&cards_lock);
        for (unsigned index = 0; index < SND_MAX_CARDS; index++) {
            const struct snd_card *card = &cards[index];
            if (card->present && card->backend->poll) card->backend->poll(card->backend->context);
        }
        lock_release(&cards_lock);
    }
}

static struct snd_card *card_of_backend(const struct snd_backend *backend) {
    for (unsigned index = 0; index < SND_MAX_CARDS; index++)
        if (cards[index].present && cards[index].backend == backend) return &cards[index];
    return NULL;
}

static void make_card_id(struct snd_card *card) {
    const char *source = card->backend->driver ? card->backend->driver : "Card";
    size_t at = 0;
    for (size_t index = 0; source[index] && at + 3U < sizeof(card->id); index++) {
        char c = source[index];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            card->id[at++] = c;
    }
    if (!at) card->id[at++] = 'C';
    for (unsigned index = 0; index < SND_MAX_CARDS; index++) {
        if (&cards[index] == card || !cards[index].present) continue;
        if (strcmp(cards[index].id, card->id) == 0) {
            card->id[at++] = '_';
            card->id[at++] = (char)('0' + card->index);
            break;
        }
    }
    card->id[at] = '\0';
}

int snd_register_card(const struct snd_backend *backend) {
    if (!backend || (!backend->streams[SND_PLAYBACK] && !backend->streams[SND_CAPTURE])) return -1;
    if (backend->control_count && (!backend->controls || !backend->control_get)) return -1;
    for (int direction = 0; direction < 2; direction++) {
        const struct snd_pcm_ops *ops = backend->streams[direction];
        if (ops && (!ops->configure || !ops->trigger || !ops->position)) return -1;
    }
    lock_acquire(&cards_lock);
    if (card_of_backend(backend)) {
        lock_release(&cards_lock);
        return -1;
    }
    struct snd_card *card = NULL;
    for (unsigned index = 0; index < SND_MAX_CARDS; index++) {
        if (cards[index].present) continue;
        card = &cards[index];
        break;
    }
    if (!card) {
        lock_release(&cards_lock);
        return -1;
    }
    struct snd_stream keep[2];
    memcpy(keep, card->streams, sizeof(keep));
    memset(card, 0, sizeof(*card));
    card->index = (int)(card - cards);
    lock_init(&card->lock, "sound card", LOCK_RANK_DEVICE);
    card->backend = backend;
    card->owner = module_active();
    for (int direction = 0; direction < 2; direction++) {
        struct snd_stream *s = &card->streams[direction];
        memcpy(s->ring_physical, keep[direction].ring_physical, sizeof(s->ring_physical));
        memcpy(s->ring_virtual, keep[direction].ring_virtual, sizeof(s->ring_virtual));
        s->ring_pages = keep[direction].ring_pages;
        s->card = card;
        s->direction = direction;
        s->ops = backend->streams[direction];
        s->state = SNDRV_PCM_STATE_OPEN;
    }
    make_card_id(card);
    card->present = 1;
    int index = card->index;
    int start_poller = backend->poll && !poller_started;
    if (start_poller) poller_started = 1;
    lock_release(&cards_lock);
    if (start_poller && !process_create_kthread("ksoundd", sound_poller, NULL)) poller_started = 0;
    devfs_publish_sound((unsigned)index, backend->streams[SND_PLAYBACK] != NULL,
                        backend->streams[SND_CAPTURE] != NULL);
    sysfs_publish_sound((unsigned)index, backend->streams[SND_PLAYBACK] != NULL,
                        backend->streams[SND_CAPTURE] != NULL);
    kprintf("SOUND: card %d \"%s\" (%s)%s%s, %u controls\n", index, backend->name,
            backend->mixer_name ? backend->mixer_name : "",
            backend->streams[SND_PLAYBACK] ? ", playback" : "",
            backend->streams[SND_CAPTURE] ? ", capture" : "", control_count(card));
    return index;
}

void snd_unregister_card(const struct snd_backend *backend) {
    lock_acquire(&cards_lock);
    struct snd_card *card = card_of_backend(backend);
    if (!card) {
        lock_release(&cards_lock);
        return;
    }
    lock_acquire(&card->lock);
    for (int direction = 0; direction < 2; direction++) {
        struct snd_stream *s = &card->streams[direction];
        if (stream_running(s) || s->state == SNDRV_PCM_STATE_PAUSED)
            stream_trigger(s, SND_TRIGGER_STOP);
        s->state = SNDRV_PCM_STATE_DISCONNECTED;
        s->generation++;
        if (s->waiters) process_wake_all(s);
    }
    card->present = 0;
    process_wake_all(card);
    int index = card->index;
    int busy = card->streams[0].open || card->streams[1].open || card->control_opens;
    if (!busy) {
        ring_release(&card->streams[0]);
        ring_release(&card->streams[1]);
    }
    lock_release(&card->lock);
    lock_release(&cards_lock);
    devfs_remove_sound((unsigned)index);
    sysfs_remove_sound((unsigned)index);
}

void snd_pcm_period_elapsed(const struct snd_backend *backend, int direction) {
    if (direction != SND_PLAYBACK && direction != SND_CAPTURE) return;
    struct snd_card *card = card_of_backend(backend);
    if (!card || !lock_try_acquire(&card->lock)) return;
    struct snd_stream *s = &card->streams[direction];
    int waiting = s->waiters != 0;
    if (card->present && s->configured) stream_update(s);
    lock_release(&card->lock);
    if (!waiting) process_wake_io();
}

void snd_ctl_notify(const struct snd_backend *backend, unsigned control, unsigned mask) {
    struct snd_card *card = card_of_backend(backend);
    if (!card) return;
    lock_acquire(&card->lock);
    if (card->present && control < control_count(card)) ctl_queue_event(card, control, mask);
    lock_release(&card->lock);
}

int sound_card_info(unsigned index, int *playback, int *capture) {
    if (index >= SND_MAX_CARDS) return 0;
    lock_acquire(&cards_lock);
    int present = cards[index].present;
    if (present) {
        *playback = cards[index].backend->streams[SND_PLAYBACK] != NULL;
        *capture = cards[index].backend->streams[SND_CAPTURE] != NULL;
    }
    lock_release(&cards_lock);
    return present;
}

int sound_card_available(void) {
    for (unsigned index = 0; index < SND_MAX_CARDS; index++)
        if (cards[index].present) return 1;
    return 0;
}

void sound_tick(void) {
    for (unsigned index = 0; index < SND_MAX_CARDS; index++) {
        struct snd_card *card = &cards[index];
        if (!card->present || !lock_try_acquire(&card->lock)) continue;
        for (int direction = 0; direction < 2; direction++) {
            struct snd_stream *s = &card->streams[direction];
            if (card->present && s->configured && stream_running(s)) stream_update(s);
        }
        lock_release(&card->lock);
    }
}

struct text_out {
    char *out;
    size_t capacity;
    size_t length;
};

static void text_add(struct text_out *text, const char *value) {
    while (value && *value) {
        if (text->length + 1U < text->capacity) text->out[text->length] = *value;
        text->length++;
        value++;
    }
    if (text->capacity)
        text->out[text->length < text->capacity ? text->length : text->capacity - 1U] = '\0';
}

static void text_number(struct text_out *text, uint64_t value) {
    char digits[24];
    unsigned at = sizeof(digits) - 1U;
    digits[at] = '\0';
    do {
        digits[--at] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && at);
    text_add(text, &digits[at]);
}

static void text_padded(struct text_out *text, uint64_t value, unsigned width) {
    char digits[24];
    unsigned at = sizeof(digits) - 1U;
    digits[at] = '\0';
    do {
        digits[--at] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && at);
    for (unsigned used = sizeof(digits) - 1U - at; used < width; used++) text_add(text, " ");
    text_add(text, &digits[at]);
}

size_t sound_proc_cards(char *out, size_t capacity) {
    struct text_out text = {out, capacity, 0};
    int any = 0;
    lock_acquire(&cards_lock);
    for (unsigned index = 0; index < SND_MAX_CARDS; index++) {
        const struct snd_card *card = &cards[index];
        if (!card->present) continue;
        any = 1;
        text_padded(&text, index, 2);
        text_add(&text, " [");
        text_add(&text, card->id);
        for (size_t pad = strlen(card->id); pad < 15U; pad++) text_add(&text, " ");
        text_add(&text, "]: ");
        text_add(&text, card->backend->driver);
        text_add(&text, " - ");
        text_add(&text, card->backend->name);
        text_add(&text, "\n                      ");
        text_add(&text, card->backend->longname ? card->backend->longname : card->backend->name);
        text_add(&text, "\n");
    }
    lock_release(&cards_lock);
    if (!any) text_add(&text, "--- no soundcards ---\n");
    return text.length;
}

size_t sound_proc_devices(char *out, size_t capacity) {
    struct text_out text = {out, capacity, 0};
    lock_acquire(&cards_lock);
    for (unsigned index = 0; index < SND_MAX_CARDS; index++) {
        const struct snd_card *card = &cards[index];
        if (!card->present) continue;
        text_padded(&text, index * 32U, 3);
        text_add(&text, ": [");
        text_padded(&text, index, 2);
        text_add(&text, "]   : control\n");
        if (card->backend->streams[SND_PLAYBACK]) {
            text_padded(&text, index * 32U + 16U, 3);
            text_add(&text, ": [");
            text_padded(&text, index, 2);
            text_add(&text, "- 0]: digital audio playback\n");
        }
        if (card->backend->streams[SND_CAPTURE]) {
            text_padded(&text, index * 32U + 24U, 3);
            text_add(&text, ": [");
            text_padded(&text, index, 2);
            text_add(&text, "- 0]: digital audio capture\n");
        }
    }
    lock_release(&cards_lock);
    return text.length;
}

size_t sound_proc_pcm(char *out, size_t capacity) {
    struct text_out text = {out, capacity, 0};
    lock_acquire(&cards_lock);
    for (unsigned index = 0; index < SND_MAX_CARDS; index++) {
        const struct snd_card *card = &cards[index];
        if (!card->present) continue;
        text_add(&text, index < 10U ? "0" : "");
        text_number(&text, index);
        text_add(&text, "-00: ");
        text_add(&text, card->backend->name);
        text_add(&text, " : ");
        text_add(&text, card->backend->name);
        if (card->backend->streams[SND_PLAYBACK]) text_add(&text, " : playback 1");
        if (card->backend->streams[SND_CAPTURE]) text_add(&text, " : capture 1");
        text_add(&text, "\n");
    }
    lock_release(&cards_lock);
    return text.length;
}

static const char *state_name(int state) {
    switch (state) {
    case SNDRV_PCM_STATE_OPEN:         return "OPEN";
    case SNDRV_PCM_STATE_SETUP:        return "SETUP";
    case SNDRV_PCM_STATE_PREPARED:     return "PREPARED";
    case SNDRV_PCM_STATE_RUNNING:      return "RUNNING";
    case SNDRV_PCM_STATE_XRUN:         return "XRUN";
    case SNDRV_PCM_STATE_DRAINING:     return "DRAINING";
    case SNDRV_PCM_STATE_PAUSED:       return "PAUSED";
    case SNDRV_PCM_STATE_SUSPENDED:    return "SUSPENDED";
    case SNDRV_PCM_STATE_DISCONNECTED: return "DISCONNECTED";
    default:                           return "UNKNOWN";
    }
}

size_t sound_proc_card_file(unsigned index, const char *path, char *out, size_t capacity) {
    struct text_out text = {out, capacity, 0};
    if (index >= SND_MAX_CARDS) return 0;
    struct snd_card *card = &cards[index];
    lock_acquire(&cards_lock);
    if (!card->present) {
        lock_release(&cards_lock);
        return 0;
    }
    if (!strcmp(path, "id")) {
        text_add(&text, card->id);
        text_add(&text, "\n");
        lock_release(&cards_lock);
        return text.length;
    }
    int direction = -1;
    const char *rest = NULL;
    if (!strncmp(path, "pcm0p/", 6)) {
        direction = SND_PLAYBACK;
        rest = path + 6;
    } else if (!strncmp(path, "pcm0c/", 6)) {
        direction = SND_CAPTURE;
        rest = path + 6;
    }
    if (direction < 0 || !card->backend->streams[direction]) {
        lock_release(&cards_lock);
        return 0;
    }
    lock_acquire(&card->lock);
    struct snd_stream *s = &card->streams[direction];
    if (!strcmp(rest, "info")) {
        text_add(&text, "card: ");
        text_number(&text, index);
        text_add(&text, "\ndevice: 0\nsubdevice: 0\nstream: ");
        text_add(&text, direction == SND_PLAYBACK ? "PLAYBACK" : "CAPTURE");
        text_add(&text, "\nid: ");
        text_add(&text, card->backend->driver);
        text_add(&text, "\nname: ");
        text_add(&text, card->backend->name);
        text_add(&text,
                 "\nsubname: subdevice #0\nclass: 0\nsubclass: 0\nsubdevices_count: 1\n"
                 "subdevices_avail: ");
        text_number(&text, s->open ? 0U : 1U);
        text_add(&text, "\n");
    } else if (!strcmp(rest, "sub0/status")) {
        if (!s->open || !s->configured || s->state == SNDRV_PCM_STATE_OPEN) {
            text_add(&text, "closed\n");
        } else {
            if (stream_running(s)) stream_update(s);
            text_add(&text, "state: ");
            text_add(&text, state_name(s->state));
            text_add(&text, "\nhw_ptr      : ");
            text_number(&text, s->hw_ptr);
            text_add(&text, "\nappl_ptr    : ");
            text_number(&text, s->appl_ptr);
            text_add(&text, "\nxruns       : ");
            text_number(&text, s->xruns);
            text_add(&text, "\n");
        }
    } else if (!strcmp(rest, "sub0/hw_params")) {
        if (!s->open || !s->configured) {
            text_add(&text, "closed\n");
        } else {
            text_add(&text, "access: ");
            text_add(&text,
                     s->access == SNDRV_PCM_ACCESS_MMAP_INTERLEAVED ? "MMAP_INTERLEAVED"
                                                                    : "RW_INTERLEAVED");
            text_add(&text, "\nformat: ");
            text_add(&text, s->format == SNDRV_PCM_FORMAT_S32_LE ? "S32_LE" : "S16_LE");
            text_add(&text, "\nsubformat: STD\nchannels: ");
            text_number(&text, s->channels);
            text_add(&text, "\nrate: ");
            text_number(&text, s->rate);
            text_add(&text, " (");
            text_number(&text, s->rate);
            text_add(&text, "/1)\nperiod_size: ");
            text_number(&text, s->period_size);
            text_add(&text, "\nbuffer_size: ");
            text_number(&text, s->buffer_size);
            text_add(&text, "\n");
        }
    } else if (!strcmp(rest, "sub0/sw_params")) {
        if (!s->open || !s->configured) {
            text_add(&text, "closed\n");
        } else {
            text_add(&text, "tstamp_mode: NONE\nperiod_step: 1\navail_min: ");
            text_number(&text, s->avail_min);
            text_add(&text, "\nstart_threshold: ");
            text_number(&text, s->start_threshold);
            text_add(&text, "\nstop_threshold: ");
            text_number(&text, s->stop_threshold);
            text_add(&text, "\nsilence_threshold: ");
            text_number(&text, s->silence_threshold);
            text_add(&text, "\nsilence_size: ");
            text_number(&text, s->silence_size);
            text_add(&text, "\nboundary: ");
            text_number(&text, s->boundary);
            text_add(&text, "\n");
        }
    }
    lock_release(&card->lock);
    lock_release(&cards_lock);
    return text.length;
}
