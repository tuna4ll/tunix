/*
 * The sound core: one card, one playback stream, ALSA's device interface.
 *
 * The shape is not a free choice. PipeWire's only backend for a PCI card is
 * its ALSA plugin, that plugin is alsa-lib, and alsa-lib talks to a kernel
 * exclusively through the ioctls on /dev/snd/pcmC0D0p and /dev/snd/controlC0.
 * So the interface here is Linux's, structure for structure, and the driver
 * underneath is reduced to four operations (see struct snd_backend).
 *
 * Two decisions are worth stating up front:
 *
 * The status and control pages are deliberately not mmap-able. alsa-lib falls
 * back to the SYNC_PTR ioctl when that mapping fails, and that fallback is the
 * only way a driver with no interrupt of its own can be correct: every read of
 * the hardware pointer happens inside a syscall, where it can be refreshed.
 * A mapped status page would go stale between periods with nothing to notice.
 *
 * The ring buffer is allocated once and kept. It is what userspace mmaps, so
 * its pages must outlive any single hw_params, and re-allocating it under a
 * live mapping is how a compositor ends up writing into freed memory.
 */
#include <stddef.h>
#include <stdint.h>

#include "../../include/file.h"
#include "../../include/hda.h"
#include "../../include/kstring.h"
#include "../../include/pmm.h"
#include "../../include/sound.h"
#include "../../include/time.h"
#include "../../include/usercopy.h"
#include "../../include/vfs.h"
#include "../../include/vmm.h"
#include "../../include/tunix/asound.h"

extern void kprintf(const char *fmt, ...);

#define EIO 5
#define ENXIO 6
#define EAGAIN 11
#define EFAULT 14
#define EBUSY 16
#define EINVAL 22
#define ENOTTY 25
#define EPIPE 32
#define EBADFD 77

/* 256 KiB of ring is a second of CD audio and four times any period PipeWire
   asks for; the pages are pinned for the life of the system. */
#define RING_PAGES 64U
#define RING_BYTES (RING_PAGES * 4096U)
/* The controller wants its cyclic buffer in whole 128-byte units. */
#define PERIOD_BYTES_ALIGN 128U
#define REFINE_PASSES 16U
#define DRAIN_MAX_NS (300ULL * 1000ULL * 1000ULL)

/* A layout change here is silent and fatal: the size of every structure is
   part of the ioctl number alsa-lib sends. */
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

static const struct snd_backend *card;

static uint64_t ring_physical[RING_PAGES];
static uint8_t *ring_virtual[RING_PAGES];
static unsigned ring_page_count;

static struct {
    int open;
    int state;
    int configured;

    uint32_t rate;
    uint32_t channels;
    uint32_t format;
    uint32_t sample_bits;
    uint32_t frame_bits;
    uint32_t frame_bytes;
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
    int tstamp_mode;

    uint64_t appl_ptr;
    uint64_t hw_ptr;
    uint64_t avail_max;
    uint64_t trigger_ns;
} pcm;

/* Mixer state, mirrored so a read does not have to ask the codec. */
static struct {
    uint32_t left;
    uint32_t right;
    int muted;
} mixer;

/* ---------------------------------------------------------------- helpers */

/* Rounding is not symmetric on purpose: a lower bound rounds down and an upper
   bound rounds up, so refining never throws away a value the hardware can
   actually produce. A divisor of zero means the bound is unknown, which is the
   loosest answer in each direction. */
static uint64_t divide_up(uint64_t value, uint64_t divisor) {
    return divisor ? (value + divisor - 1U) / divisor : UINT32_MAX;
}

static uint64_t divide_down(uint64_t value, uint64_t divisor) {
    return divisor ? value / divisor : 0;
}

static uint64_t clamp_u32(uint64_t value) {
    return value > UINT32_MAX ? UINT32_MAX : value;
}

static void copy_field(unsigned char *destination, size_t size, const char *text) {
    memset(destination, 0, size);
    if (!text) return;
    size_t index = 0;
    while (text[index] && index + 1U < size) {
        destination[index] = (unsigned char)text[index];
        index++;
    }
}

/* ------------------------------------------------------------ ring buffer */

static int ring_allocate(void) {
    if (ring_page_count) return 0;
    for (unsigned index = 0; index < RING_PAGES; index++) {
        void *physical = pmm_alloc_page();
        if (!physical) break;
        void *virtual_address = vmm_phys_to_virt((uint64_t)physical);
        if (!virtual_address) break;
        memset(virtual_address, 0, 4096);
        ring_physical[index] = (uint64_t)physical;
        ring_virtual[index] = virtual_address;
        ring_page_count++;
    }
    return ring_page_count ? 0 : -1;
}

static void ring_clear(void) {
    for (unsigned index = 0; index < ring_page_count; index++)
        memset(ring_virtual[index], 0, 4096);
}

/* Move `size` bytes into the ring at a byte offset, crossing pages. */
static void ring_store(uint32_t offset, const uint8_t *source, uint32_t size) {
    while (size) {
        uint32_t page = offset / 4096U;
        uint32_t within = offset % 4096U;
        uint32_t chunk = 4096U - within;
        if (chunk > size) chunk = size;
        if (page >= ring_page_count) return;
        memcpy(ring_virtual[page] + within, source, chunk);
        source += chunk;
        offset += chunk;
        size -= chunk;
    }
}

/* ----------------------------------------------------------- pcm pointers */

static uint64_t playback_used(void) {
    if (!pcm.boundary) return 0;
    return (pcm.appl_ptr + pcm.boundary - pcm.hw_ptr) % pcm.boundary;
}

static uint64_t playback_avail(void) {
    uint64_t used = playback_used();
    if (used > pcm.buffer_size) return pcm.buffer_size;
    return pcm.buffer_size - used;
}

/*
 * Refresh the hardware pointer from the engine.
 *
 * Called from every ioctl that reports a position, which is what makes an
 * interrupt-free driver work: the pointer is only ever read inside a syscall,
 * and it is recomputed there. The delta is taken modulo the buffer, so this
 * only stays correct while userspace syncs at least once per lap -- which is
 * exactly the condition under which the audio is not already broken.
 */
static void pcm_update_pointer(void) {
    if (!card || !pcm.buffer_size || !pcm.frame_bytes) return;
    if (pcm.state != SNDRV_PCM_STATE_RUNNING &&
        pcm.state != SNDRV_PCM_STATE_DRAINING) return;

    uint32_t position = card->position();
    uint64_t frames = position / pcm.frame_bytes;
    if (frames >= pcm.buffer_size) frames = pcm.buffer_size - 1U;
    uint64_t current = pcm.hw_ptr % pcm.buffer_size;
    uint64_t delta = (frames + pcm.buffer_size - current) % pcm.buffer_size;
    pcm.hw_ptr = (pcm.hw_ptr + delta) % pcm.boundary;

    uint64_t avail = playback_avail();
    if (avail > pcm.avail_max) pcm.avail_max = avail;

    if (pcm.state == SNDRV_PCM_STATE_DRAINING) {
        if (avail >= pcm.buffer_size) {
            (void)card->trigger(0);
            pcm.state = SNDRV_PCM_STATE_SETUP;
        }
        return;
    }
    /* The hardware ran past what was written: the ring is being replayed and
       the stream is no longer what the application thinks it is. */
    if (playback_used() > pcm.buffer_size ||
        (pcm.stop_threshold && avail >= pcm.stop_threshold)) {
        (void)card->trigger(0);
        pcm.state = SNDRV_PCM_STATE_XRUN;
    }
}

static int pcm_start(void) {
    if (!card) return -ENXIO;
    if (pcm.state != SNDRV_PCM_STATE_PREPARED) return -EBADFD;
    if (card->trigger(1) != 0) return -EIO;
    pcm.state = SNDRV_PCM_STATE_RUNNING;
    pcm.trigger_ns = time_uptime_ns();
    return 0;
}

static void pcm_stop(int state) {
    if (card && (pcm.state == SNDRV_PCM_STATE_RUNNING ||
                 pcm.state == SNDRV_PCM_STATE_DRAINING ||
                 pcm.state == SNDRV_PCM_STATE_PAUSED))
        (void)card->trigger(0);
    pcm.state = state;
}

static int pcm_prepare(void) {
    if (!card || !pcm.configured) return -EBADFD;
    pcm_stop(SNDRV_PCM_STATE_SETUP);
    if (card->prepare && card->prepare() != 0) return -EIO;
    ring_clear();
    pcm.appl_ptr = 0;
    pcm.hw_ptr = 0;
    pcm.avail_max = pcm.buffer_size;
    pcm.state = SNDRV_PCM_STATE_PREPARED;
    return 0;
}

/* Start once the application has queued as much as it asked to queue. */
static void pcm_maybe_start(void) {
    if (pcm.state != SNDRV_PCM_STATE_PREPARED) return;
    if (playback_used() >= pcm.start_threshold) (void)pcm_start();
}

/* ------------------------------------------------------- hw_params refine */

static struct snd_interval *interval_of(struct snd_pcm_hw_params *params, unsigned var) {
    return &params->intervals[var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static struct snd_mask *mask_of(struct snd_pcm_hw_params *params, unsigned var) {
    return &params->masks[var];
}

static int mask_test(const struct snd_mask *mask, unsigned bit) {
    if (bit >= SNDRV_MASK_BITS) return 0;
    return (mask->bits[bit / 32U] >> (bit % 32U)) & 1U;
}

static int mask_intersect(struct snd_mask *mask, const uint32_t *allowed) {
    int changed = 0;
    for (unsigned word = 0; word < SNDRV_MASK_WORDS; word++) {
        uint32_t value = mask->bits[word] & allowed[word];
        if (value != mask->bits[word]) changed = 1;
        mask->bits[word] = value;
    }
    return changed;
}

static int mask_empty(const struct snd_mask *mask) {
    for (unsigned word = 0; word < SNDRV_MASK_WORDS; word++)
        if (mask->bits[word]) return 0;
    return 1;
}

/* Tighten an interval to [low, high]. All of ours are integer intervals, so
   open bounds are folded into closed ones once, here. */
static int interval_refine(struct snd_interval *interval, uint64_t low, uint64_t high) {
    uint64_t min = interval->min;
    uint64_t max = interval->max;
    if (interval->openmin) min += 1U;
    if (interval->openmax) max = max ? max - 1U : 0U;
    if (low > min) min = low;
    if (high < max) max = high;

    int changed = interval->openmin || interval->openmax || !interval->integer ||
                  min != interval->min || max != interval->max;
    interval->min = (unsigned)clamp_u32(min);
    interval->max = (unsigned)clamp_u32(max);
    interval->openmin = 0;
    interval->openmax = 0;
    interval->integer = 1;
    if (interval->min > interval->max) interval->empty = 1;
    return changed;
}

/* c = a * b / k, refined from all three sides. */
static int relate_muldiv(struct snd_interval *c, struct snd_interval *a,
                         struct snd_interval *b, uint64_t k) {
    int changed = 0;
    changed |= interval_refine(c, divide_down((uint64_t)a->min * b->min, k),
                               divide_up((uint64_t)a->max * b->max, k));
    changed |= interval_refine(a, divide_down((uint64_t)c->min * k, b->max),
                               divide_up((uint64_t)c->max * k, b->min));
    changed |= interval_refine(b, divide_down((uint64_t)c->min * k, a->max),
                               divide_up((uint64_t)c->max * k, a->min));
    return changed;
}

/* Bits per sample for a format, or zero if this driver cannot play it. */
static uint32_t format_bits(unsigned format) {
    switch (format) {
    case SNDRV_PCM_FORMAT_S16_LE: return 16;
    case SNDRV_PCM_FORMAT_S32_LE: return 32;
    default: return 0;
    }
}

static void supported_access_mask(uint32_t *bits) {
    memset(bits, 0, SNDRV_MASK_WORDS * sizeof(bits[0]));
    bits[0] = (1U << SNDRV_PCM_ACCESS_MMAP_INTERLEAVED) |
              (1U << SNDRV_PCM_ACCESS_RW_INTERLEAVED);
}

static void supported_format_mask(uint32_t *bits) {
    memset(bits, 0, SNDRV_MASK_WORDS * sizeof(bits[0]));
    for (unsigned format = 0; format < 32U; format++)
        if ((card->hardware.formats >> format) & 1U) bits[0] |= 1U << format;
}

static void supported_subformat_mask(uint32_t *bits) {
    memset(bits, 0, SNDRV_MASK_WORDS * sizeof(bits[0]));
    bits[0] = 1U << SNDRV_PCM_SUBFORMAT_STD;
}

/* Pull the rate interval onto the rates the link can actually clock. */
static int refine_rate_list(struct snd_interval *rate) {
    if (!card->hardware.rate_count) return 0;
    uint64_t low = UINT32_MAX;
    uint64_t high = 0;
    for (unsigned index = 0; index < card->hardware.rate_count; index++) {
        uint32_t value = card->hardware.rates[index];
        if (value < rate->min || value > rate->max) continue;
        if (value < low) low = value;
        if (value > high) high = value;
    }
    if (!high) {
        rate->empty = 1;
        return 1;
    }
    return interval_refine(rate, low, high);
}

static int rate_supported(uint32_t rate) {
    if (!card->hardware.rate_count) return 1;
    for (unsigned index = 0; index < card->hardware.rate_count; index++)
        if (card->hardware.rates[index] == rate) return 1;
    return 0;
}

static uint32_t ring_limit_bytes(void) {
    uint32_t limit = ring_page_count * 4096U;
    if (card->hardware.buffer_bytes_max &&
        card->hardware.buffer_bytes_max < limit)
        limit = card->hardware.buffer_bytes_max;
    return limit;
}

/*
 * Narrow a parameter set to what the card can do.
 *
 * Every relation between the parameters is stated once and then iterated to a
 * fixed point, because they are circular: period_bytes constrains period_size,
 * which constrains buffer_size through periods, which constrains buffer_bytes,
 * which constrains period_bytes again. One pass leaves the set inconsistent
 * and alsa-lib then asks for a value the hardware was never offered.
 */
static int hw_refine(struct snd_pcm_hw_params *params) {
    const struct snd_hardware *hardware = &card->hardware;
    uint32_t allowed[SNDRV_MASK_WORDS];
    /* Kept to answer the only question the caller asks afterwards: which of
       these parameters did you change. See the end of this function. */
    struct snd_pcm_hw_params before = *params;

    supported_access_mask(allowed);
    (void)mask_intersect(mask_of(params, SNDRV_PCM_HW_PARAM_ACCESS), allowed);
    supported_format_mask(allowed);
    (void)mask_intersect(mask_of(params, SNDRV_PCM_HW_PARAM_FORMAT), allowed);
    supported_subformat_mask(allowed);
    (void)mask_intersect(mask_of(params, SNDRV_PCM_HW_PARAM_SUBFORMAT), allowed);

    for (unsigned index = 0; index < SNDRV_PCM_HW_PARAM_MASK_COUNT; index++)
        if (mask_empty(&params->masks[index])) return -EINVAL;

    struct snd_interval *sample_bits = interval_of(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS);
    struct snd_interval *frame_bits = interval_of(params, SNDRV_PCM_HW_PARAM_FRAME_BITS);
    struct snd_interval *channels = interval_of(params, SNDRV_PCM_HW_PARAM_CHANNELS);
    struct snd_interval *rate = interval_of(params, SNDRV_PCM_HW_PARAM_RATE);
    struct snd_interval *period_time = interval_of(params, SNDRV_PCM_HW_PARAM_PERIOD_TIME);
    struct snd_interval *period_size = interval_of(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
    struct snd_interval *period_bytes = interval_of(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES);
    struct snd_interval *periods = interval_of(params, SNDRV_PCM_HW_PARAM_PERIODS);
    struct snd_interval *buffer_time = interval_of(params, SNDRV_PCM_HW_PARAM_BUFFER_TIME);
    struct snd_interval *buffer_size = interval_of(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
    struct snd_interval *buffer_bytes = interval_of(params, SNDRV_PCM_HW_PARAM_BUFFER_BYTES);
    struct snd_interval *tick_time = interval_of(params, SNDRV_PCM_HW_PARAM_TICK_TIME);

    /* Sample width comes from the formats still on the table. */
    uint64_t bits_low = UINT32_MAX;
    uint64_t bits_high = 0;
    for (unsigned format = 0; format < 32U; format++) {
        if (!mask_test(mask_of(params, SNDRV_PCM_HW_PARAM_FORMAT), format)) continue;
        uint32_t width = format_bits(format);
        if (!width) continue;
        if (width < bits_low) bits_low = width;
        if (width > bits_high) bits_high = width;
    }
    if (!bits_high) return -EINVAL;

    uint32_t ring_bytes = ring_limit_bytes();
    (void)interval_refine(tick_time, 0, UINT32_MAX);

    for (unsigned pass = 0; pass < REFINE_PASSES; pass++) {
        int changed = 0;
        changed |= interval_refine(sample_bits, bits_low, bits_high);
        changed |= interval_refine(channels, hardware->channels_min,
                                   hardware->channels_max);
        changed |= interval_refine(rate, hardware->rate_min, hardware->rate_max);
        changed |= refine_rate_list(rate);
        changed |= interval_refine(periods, hardware->periods_min,
                                   hardware->periods_max);
        changed |= interval_refine(period_bytes, hardware->period_bytes_min,
                                   hardware->period_bytes_max);
        /* The step the controller wants, as bounds: a single-valued request
           that is not a multiple is rejected outright at hw_params. */
        changed |= interval_refine(period_bytes,
            divide_up(period_bytes->min, PERIOD_BYTES_ALIGN) * PERIOD_BYTES_ALIGN,
            divide_down(period_bytes->max, PERIOD_BYTES_ALIGN) * PERIOD_BYTES_ALIGN);
        changed |= interval_refine(buffer_bytes, PERIOD_BYTES_ALIGN, ring_bytes);

        changed |= relate_muldiv(frame_bits, sample_bits, channels, 1);
        changed |= relate_muldiv(period_bytes, period_size, frame_bits, 8);
        changed |= relate_muldiv(buffer_bytes, buffer_size, frame_bits, 8);
        changed |= relate_muldiv(buffer_size, period_size, periods, 1);
        /*
         * And the part that is not a bound: the buffer is a whole number of
         * periods, so with a period fixed, a buffer that is not a multiple of
         * it does not exist.
         *
         * relate_muldiv() alone says only that 1536 frames lies between two
         * and three 528-frame periods, which is true and useless. Answering
         * that leaves alsa-lib holding a set with no solution in it, and it
         * does not find that out until it commits -- so the failure surfaces
         * as snd_pcm_hw_params() returning EINVAL with no ioctl behind it,
         * from a library that was told the configuration was available.
         * Refusing it here is what sends the caller back to ask for a buffer
         * that exists, which is what every program does next.
         */
        if (!period_size->empty && period_size->min == period_size->max &&
            period_size->min) {
            uint64_t one = period_size->min;
            changed |= interval_refine(buffer_size,
                                       divide_up(buffer_size->min, one) * one,
                                       divide_down(buffer_size->max, one) * one);
        }
        /* The two time parameters, in microseconds, both ways. */
        changed |= interval_refine(period_time,
                                   divide_down((uint64_t)period_size->min * 1000000U, rate->max),
                                   divide_up((uint64_t)period_size->max * 1000000U, rate->min));
        changed |= interval_refine(period_size,
                                   divide_down((uint64_t)period_time->min * rate->min, 1000000U),
                                   divide_up((uint64_t)period_time->max * rate->max, 1000000U));
        changed |= interval_refine(buffer_time,
                                   divide_down((uint64_t)buffer_size->min * 1000000U, rate->max),
                                   divide_up((uint64_t)buffer_size->max * 1000000U, rate->min));
        changed |= interval_refine(buffer_size,
                                   divide_down((uint64_t)buffer_time->min * rate->min, 1000000U),
                                   divide_up((uint64_t)buffer_time->max * rate->max, 1000000U));

        for (unsigned index = 0; index < SNDRV_PCM_HW_PARAM_INTERVAL_COUNT; index++)
            if (params->intervals[index].empty) return -EINVAL;
        if (!changed) break;
    }

    params->info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
                   SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER;
    params->msbits = sample_bits->max;
    params->rate_num = rate->max;
    params->rate_den = 1;
    params->fifo_size = hardware->fifo_size;

    /*
     * Which parameters this call actually narrowed, one bit each.
     *
     * It used to answer `cmask = rmask`: "everything you asked about changed",
     * which is not an approximation, it is a different statement. Opening the
     * card directly survives it because the hw plugin takes the answer and
     * asks nothing more. A plugin chain does not: alsa-lib refines the slave,
     * maps whatever cmask names back up through the chain, and repeats until
     * nothing changes. Told that everything changes, it re-derives the client
     * parameters from a slave that never moved, gets a set it cannot satisfy,
     * and gives up in the library -- `snd_pcm_hw_params` returning EINVAL with
     * no ioctl behind it, which is a hard thing to see from in here.
     *
     * What that looked like: `aplay -D hw:0,0` worked and `aplay -D default`
     * did not, and neither did anything else that goes through `plug`, which
     * is nearly every program. OpenAL, and so SuperTuxKart, is in that group.
     */
    uint32_t changed_mask = 0;
    for (unsigned index = 0; index < SNDRV_PCM_HW_PARAM_MASK_COUNT; index++) {
        for (unsigned word = 0; word < SNDRV_MASK_WORDS; word++)
            if (before.masks[index].bits[word] != params->masks[index].bits[word]) {
                changed_mask |= 1U << (SNDRV_PCM_HW_PARAM_FIRST_MASK + index);
                break;
            }
    }
    for (unsigned index = 0; index < SNDRV_PCM_HW_PARAM_INTERVAL_COUNT; index++) {
        const struct snd_interval *was = &before.intervals[index];
        const struct snd_interval *now = &params->intervals[index];
        if (was->min != now->min || was->max != now->max ||
            was->openmin != now->openmin || was->openmax != now->openmax ||
            was->integer != now->integer || was->empty != now->empty)
            changed_mask |= 1U << (SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + index);
    }
    params->cmask = changed_mask;
    /* Consumed: the request has been answered, and leaving it set makes the
       next caller's refine look like a repeat of this one. */
    params->rmask = 0;
    return 0;
}

static int interval_single(const struct snd_interval *interval, uint64_t *out) {
    if (interval->empty || interval->min != interval->max) return -1;
    *out = interval->min;
    return 0;
}

static unsigned mask_single(const struct snd_mask *mask) {
    unsigned found = SNDRV_MASK_BITS;
    for (unsigned bit = 0; bit < SNDRV_MASK_BITS; bit++) {
        if (!mask_test(mask, bit)) continue;
        if (found != SNDRV_MASK_BITS) return SNDRV_MASK_BITS;
        found = bit;
    }
    return found;
}

/* Commit a fully determined parameter set to the hardware. */
static int hw_params(struct snd_pcm_hw_params *params) {
    if (hw_refine(params) != 0) return -EINVAL;

    unsigned access = mask_single(mask_of(params, SNDRV_PCM_HW_PARAM_ACCESS));
    unsigned format = mask_single(mask_of(params, SNDRV_PCM_HW_PARAM_FORMAT));
    if (access == SNDRV_MASK_BITS || format == SNDRV_MASK_BITS) return -EINVAL;

    uint64_t channels, rate, period_size, periods, buffer_size;
    uint64_t period_bytes, buffer_bytes, frame_bits;
    if (interval_single(interval_of(params, SNDRV_PCM_HW_PARAM_CHANNELS), &channels) ||
        interval_single(interval_of(params, SNDRV_PCM_HW_PARAM_RATE), &rate) ||
        interval_single(interval_of(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE), &period_size) ||
        interval_single(interval_of(params, SNDRV_PCM_HW_PARAM_PERIODS), &periods) ||
        interval_single(interval_of(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE), &buffer_size) ||
        interval_single(interval_of(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES), &period_bytes) ||
        interval_single(interval_of(params, SNDRV_PCM_HW_PARAM_BUFFER_BYTES), &buffer_bytes) ||
        interval_single(interval_of(params, SNDRV_PCM_HW_PARAM_FRAME_BITS), &frame_bits))
        return -EINVAL;

    if (!rate_supported((uint32_t)rate)) return -EINVAL;
    if (period_bytes % PERIOD_BYTES_ALIGN) return -EINVAL;
    if (buffer_bytes % PERIOD_BYTES_ALIGN) return -EINVAL;
    if (buffer_bytes > ring_limit_bytes()) return -EINVAL;
    if (frame_bits % 8U) return -EINVAL;

    pcm_stop(SNDRV_PCM_STATE_SETUP);

    struct snd_stream_format stream = {
        .rate = (uint32_t)rate,
        .channels = (uint32_t)channels,
        .format = format
    };
    unsigned pages = (unsigned)divide_up(buffer_bytes, 4096U);
    if (pages > ring_page_count) return -EINVAL;
    if (card->configure(&stream, ring_physical, pages, (uint32_t)buffer_bytes) != 0)
        return -EINVAL;

    pcm.rate = (uint32_t)rate;
    pcm.channels = (uint32_t)channels;
    pcm.format = format;
    pcm.sample_bits = format_bits(format);
    pcm.frame_bits = (uint32_t)frame_bits;
    pcm.frame_bytes = (uint32_t)(frame_bits / 8U);
    pcm.period_size = period_size;
    pcm.periods = periods;
    pcm.buffer_size = buffer_size;
    pcm.period_bytes = (uint32_t)period_bytes;
    pcm.buffer_bytes = (uint32_t)buffer_bytes;

    /* ALSA's boundary: the largest power-of-two multiple of the buffer that
       still leaves room to add a buffer without overflowing a long. */
    pcm.boundary = buffer_size;
    while (pcm.boundary * 2U <= (uint64_t)INT64_MAX - buffer_size)
        pcm.boundary *= 2U;

    pcm.avail_min = period_size;
    pcm.start_threshold = 1;
    pcm.stop_threshold = buffer_size;
    pcm.silence_threshold = 0;
    pcm.silence_size = 0;
    pcm.appl_ptr = 0;
    pcm.hw_ptr = 0;
    pcm.avail_max = buffer_size;
    pcm.configured = 1;
    pcm.state = SNDRV_PCM_STATE_SETUP;
    (void)access;
    return 0;
}

static int sw_params(struct snd_pcm_sw_params *params) {
    if (!pcm.configured) return -EBADFD;
    if (!params->avail_min) return -EINVAL;
    if (params->silence_size > pcm.boundary) return -EINVAL;
    pcm.avail_min = params->avail_min;
    pcm.start_threshold = params->start_threshold;
    pcm.stop_threshold = params->stop_threshold;
    pcm.silence_threshold = params->silence_threshold;
    pcm.silence_size = params->silence_size;
    pcm.tstamp_mode = params->tstamp_mode;
    /* The boundary belongs to the kernel; userspace is told what it is. */
    params->boundary = pcm.boundary;
    return 0;
}

/* -------------------------------------------------------------- transfers */

static int64_t pcm_append(const uint8_t *source, uint64_t frames) {
    if (!pcm.configured) return -EBADFD;
    if (pcm.state == SNDRV_PCM_STATE_XRUN) return -EPIPE;
    if (pcm.state == SNDRV_PCM_STATE_SETUP) {
        int status = pcm_prepare();
        if (status != 0) return status;
    }
    pcm_update_pointer();
    if (pcm.state == SNDRV_PCM_STATE_XRUN) return -EPIPE;

    uint64_t avail = playback_avail();
    if (!avail) return -EAGAIN;
    if (frames > avail) frames = avail;

    uint32_t offset = (uint32_t)((pcm.appl_ptr % pcm.buffer_size) * pcm.frame_bytes);
    ring_store(offset, source, (uint32_t)(frames * pcm.frame_bytes));
    pcm.appl_ptr = (pcm.appl_ptr + frames) % pcm.boundary;
    pcm_maybe_start();
    return (int64_t)frames;
}

int64_t sound_pcm_write(struct vfs_node *node, uint64_t offset, size_t size,
                        const void *buffer) {
    (void)node;
    (void)offset;
    if (!card || !buffer) return -ENXIO;
    if (!pcm.configured || !pcm.frame_bytes) return -EBADFD;
    uint64_t frames = size / pcm.frame_bytes;
    if (!frames) return -EINVAL;
    int64_t moved = pcm_append(buffer, frames);
    if (moved < 0) return moved;
    return moved * (int64_t)pcm.frame_bytes;
}

/* The buffer has room for at least avail_min frames, so a blocked write or a
   poll for POLLOUT can make progress. */
int sound_pcm_write_ready(struct vfs_node *node) {
    (void)node;
    if (!card) return 1;
    if (!pcm.configured) return 1;
    pcm_update_pointer();
    if (pcm.state == SNDRV_PCM_STATE_XRUN ||
        pcm.state == SNDRV_PCM_STATE_SETUP ||
        pcm.state == SNDRV_PCM_STATE_PREPARED) return 1;
    uint64_t minimum = pcm.avail_min ? pcm.avail_min : 1U;
    return playback_avail() >= minimum;
}

/* ---------------------------------------------------------------- ioctls */

static void fill_status(struct snd_pcm_status *status) {
    memset(status, 0, sizeof(*status));
    pcm_update_pointer();
    uint64_t now = time_uptime_ns();
    status->state = pcm.state;
    status->trigger_tstamp.tv_sec = (int64_t)(pcm.trigger_ns / 1000000000ULL);
    status->trigger_tstamp.tv_nsec = (int64_t)(pcm.trigger_ns % 1000000000ULL);
    status->tstamp.tv_sec = (int64_t)(now / 1000000000ULL);
    status->tstamp.tv_nsec = (int64_t)(now % 1000000000ULL);
    status->appl_ptr = pcm.appl_ptr;
    status->hw_ptr = pcm.hw_ptr;
    status->delay = (long)playback_used();
    status->avail = playback_avail();
    status->avail_max = pcm.avail_max;
    status->suspended_state = SNDRV_PCM_STATE_SUSPENDED;
}

/*
 * The pointer exchange, and the first thing alsa-lib asks for.
 *
 * It must answer before hw_params has ever been called: with the status page
 * unmapped, alsa-lib reads the stream state through this ioctl, and it does so
 * the moment the device is opened. Refusing an unconfigured stream here is a
 * failed snd_pcm_open, several layers away from anything that mentions sync.
 */
static int64_t ioctl_sync_ptr(uint64_t user_argument) {
    struct snd_pcm_sync_ptr sync;
    if (copy_from_user(&sync, user_argument, sizeof(sync)) != 0) return -EFAULT;

    if (!(sync.flags & SNDRV_PCM_SYNC_PTR_AVAIL_MIN))
        pcm.avail_min = sync.c.control.avail_min;
    /* An application pointer means nothing until there is a ring to place it
       in, and the wrap would be a division by zero. */
    if (pcm.boundary) {
        if (!(sync.flags & SNDRV_PCM_SYNC_PTR_APPL))
            pcm.appl_ptr = sync.c.control.appl_ptr % pcm.boundary;
        pcm_update_pointer();
        pcm_maybe_start();
    }

    uint64_t now = time_uptime_ns();
    memset(&sync.s, 0, sizeof(sync.s));
    memset(&sync.c, 0, sizeof(sync.c));
    sync.s.status.state = pcm.state;
    sync.s.status.hw_ptr = pcm.hw_ptr;
    sync.s.status.tstamp.tv_sec = (int64_t)(now / 1000000000ULL);
    sync.s.status.tstamp.tv_nsec = (int64_t)(now % 1000000000ULL);
    sync.s.status.suspended_state = SNDRV_PCM_STATE_SUSPENDED;
    sync.c.control.appl_ptr = pcm.appl_ptr;
    sync.c.control.avail_min = pcm.avail_min;
    return copy_to_user(user_argument, &sync, sizeof(sync)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_xferi(uint64_t user_argument) {
    struct snd_xferi transfer;
    if (copy_from_user(&transfer, user_argument, sizeof(transfer)) != 0) return -EFAULT;
    if (!pcm.configured || !pcm.frame_bytes) return -EBADFD;

    uint8_t staging[4096];
    uint64_t remaining = transfer.frames;
    uint64_t done = 0;
    uint64_t source = transfer.buf;
    uint32_t per_pass = (uint32_t)(sizeof(staging) / pcm.frame_bytes);
    if (!per_pass) return -EINVAL;

    while (remaining) {
        uint64_t chunk = remaining < per_pass ? remaining : per_pass;
        if (copy_from_user(staging, source, (size_t)(chunk * pcm.frame_bytes)) != 0) {
            if (!done) return -EFAULT;
            break;
        }
        int64_t moved = pcm_append(staging, chunk);
        if (moved < 0) {
            if (done) break;
            transfer.result = moved;
            (void)copy_to_user(user_argument, &transfer, sizeof(transfer));
            return moved;
        }
        done += (uint64_t)moved;
        source += (uint64_t)moved * pcm.frame_bytes;
        remaining -= (uint64_t)moved;
        if ((uint64_t)moved < chunk) break;
    }
    transfer.result = (long)done;
    if (copy_to_user(user_argument, &transfer, sizeof(transfer)) != 0) return -EFAULT;
    return (int64_t)done;
}

/*
 * Wait out what is still queued.
 *
 * Bounded, and by the queue itself: there is at most one ring of audio left,
 * so the spin is the time that audio takes and never longer. Anything still
 * playing after that is a stopped engine, and the state moves on regardless.
 */
static int64_t ioctl_drain(void) {
    if (!pcm.configured) return -EBADFD;
    /* Data queued but under the start threshold still has to be played. */
    if (pcm.state == SNDRV_PCM_STATE_PREPARED && playback_used())
        (void)pcm_start();
    if (pcm.state != SNDRV_PCM_STATE_RUNNING) {
        pcm_stop(SNDRV_PCM_STATE_SETUP);
        return 0;
    }
    pcm.state = SNDRV_PCM_STATE_DRAINING;
    uint64_t deadline = time_uptime_ns() + DRAIN_MAX_NS;
    while (pcm.state == SNDRV_PCM_STATE_DRAINING) {
        pcm_update_pointer();
        if (time_uptime_ns() >= deadline) break;
        __asm__ volatile("pause");
    }
    pcm_stop(SNDRV_PCM_STATE_SETUP);
    return 0;
}

static int64_t ioctl_channel_info(uint64_t user_argument) {
    struct snd_pcm_channel_info info;
    if (copy_from_user(&info, user_argument, sizeof(info)) != 0) return -EFAULT;
    if (!pcm.configured) return -EBADFD;
    if (info.channel >= pcm.channels) return -EINVAL;
    /* Interleaved: one mapping for every channel, differing only in where the
       first sample of the channel sits inside a frame. */
    info.offset = 0;
    info.first = info.channel * pcm.sample_bits;
    info.step = pcm.frame_bits;
    return copy_to_user(user_argument, &info, sizeof(info)) == 0 ? 0 : -EFAULT;
}

static void fill_pcm_info(struct snd_pcm_info *info) {
    memset(info, 0, sizeof(*info));
    info->device = 0;
    info->subdevice = 0;
    info->stream = SNDRV_PCM_STREAM_PLAYBACK;
    info->card = 0;
    copy_field(info->id, sizeof(info->id), card->driver);
    copy_field(info->name, sizeof(info->name), card->name);
    copy_field(info->subname, sizeof(info->subname), "subdevice #0");
    info->subdevices_count = 1;
    info->subdevices_avail = pcm.open ? 0U : 1U;
}

int64_t sound_pcm_ioctl(struct vfs_node *node, unsigned long request,
                        uint64_t user_argument) {
    (void)node;
    if (!card) return -ENXIO;
    if (SND_IOC_TYPE(request) != 'A') return -ENOTTY;

    switch (SND_IOC_NR(request)) {
    case 0x00: {  /* PVERSION */
        int version = SNDRV_PCM_VERSION;
        return copy_to_user(user_argument, &version, sizeof(version)) == 0 ? 0 : -EFAULT;
    }
    case 0x01: {  /* INFO */
        struct snd_pcm_info info;
        fill_pcm_info(&info);
        return copy_to_user(user_argument, &info, sizeof(info)) == 0 ? 0 : -EFAULT;
    }
    case 0x02:    /* TSTAMP */
    case 0x03:    /* TTSTAMP */
    case 0x04:    /* USER_PVERSION */
        return 0;
    case SNDRV_PCM_IOCTL_NR_HW_REFINE:
    case SNDRV_PCM_IOCTL_NR_HW_PARAMS: {
        struct snd_pcm_hw_params params;
        if (copy_from_user(&params, user_argument, sizeof(params)) != 0) return -EFAULT;
        int64_t result = SND_IOC_NR(request) == SNDRV_PCM_IOCTL_NR_HW_REFINE
                             ? hw_refine(&params) : hw_params(&params);
        if (result != 0) return result;
        return copy_to_user(user_argument, &params, sizeof(params)) == 0 ? 0 : -EFAULT;
    }
    case SNDRV_PCM_IOCTL_NR_HW_FREE:
        pcm_stop(SNDRV_PCM_STATE_OPEN);
        pcm.configured = 0;
        return 0;
    case SNDRV_PCM_IOCTL_NR_SW_PARAMS: {
        struct snd_pcm_sw_params params;
        if (copy_from_user(&params, user_argument, sizeof(params)) != 0) return -EFAULT;
        int64_t result = sw_params(&params);
        if (result != 0) return result;
        return copy_to_user(user_argument, &params, sizeof(params)) == 0 ? 0 : -EFAULT;
    }
    case SNDRV_PCM_IOCTL_NR_STATUS:
    case SNDRV_PCM_IOCTL_NR_STATUS_EXT: {
        struct snd_pcm_status status;
        fill_status(&status);
        return copy_to_user(user_argument, &status, sizeof(status)) == 0 ? 0 : -EFAULT;
    }
    case SNDRV_PCM_IOCTL_NR_DELAY: {
        pcm_update_pointer();
        long delay = (long)playback_used();
        return copy_to_user(user_argument, &delay, sizeof(delay)) == 0 ? 0 : -EFAULT;
    }
    case SNDRV_PCM_IOCTL_NR_HWSYNC:
        pcm_update_pointer();
        return 0;
    case SNDRV_PCM_IOCTL_NR_SYNC_PTR:
        return ioctl_sync_ptr(user_argument);
    case SNDRV_PCM_IOCTL_NR_CHANNEL_INFO:
        return ioctl_channel_info(user_argument);
    case SNDRV_PCM_IOCTL_NR_PREPARE:
        return pcm_prepare();
    case SNDRV_PCM_IOCTL_NR_RESET:
        pcm.appl_ptr = pcm.hw_ptr;
        return 0;
    case SNDRV_PCM_IOCTL_NR_START:
        return pcm_start();
    case SNDRV_PCM_IOCTL_NR_DROP:
        pcm_stop(SNDRV_PCM_STATE_SETUP);
        return 0;
    case SNDRV_PCM_IOCTL_NR_DRAIN:
        return ioctl_drain();
    case SNDRV_PCM_IOCTL_NR_PAUSE:
        return -EINVAL;
    case SNDRV_PCM_IOCTL_NR_XRUN:
        pcm_stop(SNDRV_PCM_STATE_XRUN);
        return 0;
    case SNDRV_PCM_IOCTL_NR_WRITEI_FRAMES:
        return ioctl_xferi(user_argument);
    case SNDRV_PCM_IOCTL_NR_LINK:
    case SNDRV_PCM_IOCTL_NR_UNLINK:
        return -EINVAL;
    default:
        return -ENOTTY;
    }
}

/*
 * mmap of the ring, and only of the ring.
 *
 * The status and control pages are refused on purpose; see the note at the
 * top of this file. alsa-lib treats the failure as a signal to use SYNC_PTR,
 * which is the path this driver can keep honest.
 */
int64_t sound_pcm_mmap(struct vfs_node *node, struct file *file, uint64_t cr3,
                       uint64_t virtual_address, uint64_t length,
                       uint64_t offset, uint64_t page_flags) {
    (void)node;
    (void)file;
    if (!card || !length) return -EINVAL;
    if (offset != SNDRV_PCM_MMAP_OFFSET_DATA) return -ENXIO;
    if (length > (uint64_t)ring_page_count * 4096ULL) return -EINVAL;

    uint64_t flags = page_flags | PAGE_USER | PAGE_PRESENT | PAGE_NX | PAGE_SHARED;
    uint64_t mapped = 0;
    for (; mapped < length; mapped += 4096ULL) {
        uint64_t physical = ring_physical[mapped / 4096ULL];
        if (pmm_page_ref(physical) != 0 ||
            vmm_map_page_in(cr3, virtual_address + mapped, physical, flags) != 0) {
            while (mapped) {
                mapped -= 4096ULL;
                (void)vmm_unmap_page_in(cr3, virtual_address + mapped);
                pmm_free_page((void *)ring_physical[mapped / 4096ULL]);
            }
            return -EINVAL;
        }
    }
    return 0;
}

void sound_pcm_open(struct vfs_node *node) {
    (void)node;
    pcm.open++;
    if (pcm.open == 1) pcm.state = SNDRV_PCM_STATE_OPEN;
}

void sound_pcm_close(struct vfs_node *node) {
    (void)node;
    if (pcm.open) pcm.open--;
    if (pcm.open) return;
    pcm_stop(SNDRV_PCM_STATE_OPEN);
    pcm.configured = 0;
}

/* --------------------------------------------------------- control device */

#define CTL_NUMID_VOLUME 1U
#define CTL_NUMID_SWITCH 2U

static unsigned control_element_count(void) {
    return card && card->set_volume ? 2U : 0U;
}

static void fill_element_id(struct snd_ctl_elem_id *id, unsigned numid) {
    memset(id, 0, sizeof(*id));
    id->numid = numid;
    id->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    copy_field(id->name, sizeof(id->name),
               numid == CTL_NUMID_VOLUME ? "Master Playback Volume"
                                         : "Master Playback Switch");
}

/* Match by numid, or by name when userspace has none yet. */
static unsigned element_lookup(const struct snd_ctl_elem_id *id) {
    if (id->numid == CTL_NUMID_VOLUME || id->numid == CTL_NUMID_SWITCH)
        return id->numid;
    const char *name = (const char *)id->name;
    size_t limit = sizeof(id->name);
    if (!strncmp(name, "Master Playback Volume", limit)) return CTL_NUMID_VOLUME;
    if (!strncmp(name, "Master Playback Switch", limit)) return CTL_NUMID_SWITCH;
    return 0;
}

static int64_t ioctl_elem_list(uint64_t user_argument) {
    struct snd_ctl_elem_list list;
    if (copy_from_user(&list, user_argument, sizeof(list)) != 0) return -EFAULT;
    unsigned total = control_element_count();
    list.count = total;
    list.used = 0;
    for (unsigned index = list.offset; index < total && list.used < list.space; index++) {
        struct snd_ctl_elem_id id;
        fill_element_id(&id, index + 1U);
        if (copy_to_user(list.pids + (uint64_t)list.used * sizeof(id), &id,
                         sizeof(id)) != 0) return -EFAULT;
        list.used++;
    }
    return copy_to_user(user_argument, &list, sizeof(list)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_elem_info(uint64_t user_argument) {
    struct snd_ctl_elem_info info;
    if (copy_from_user(&info, user_argument, sizeof(info)) != 0) return -EFAULT;
    unsigned numid = element_lookup(&info.id);
    if (!numid || !control_element_count()) return -ENXIO;

    memset(&info.value, 0, sizeof(info.value));
    fill_element_id(&info.id, numid);
    info.access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
    info.count = 2;
    info.owner = 0;
    if (numid == CTL_NUMID_VOLUME) {
        info.type = SNDRV_CTL_ELEM_TYPE_INTEGER;
        info.value.integer.min = 0;
        info.value.integer.max = (long)card->volume_max;
        info.value.integer.step = 1;
    } else {
        info.type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
        info.value.integer.min = 0;
        info.value.integer.max = 1;
        info.value.integer.step = 1;
    }
    return copy_to_user(user_argument, &info, sizeof(info)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_elem_read(uint64_t user_argument) {
    struct snd_ctl_elem_value value;
    if (copy_from_user(&value, user_argument, sizeof(value)) != 0) return -EFAULT;
    unsigned numid = element_lookup(&value.id);
    if (!numid || !control_element_count()) return -ENXIO;
    fill_element_id(&value.id, numid);
    if (numid == CTL_NUMID_VOLUME) {
        value.value.integer_value[0] = (long)mixer.left;
        value.value.integer_value[1] = (long)mixer.right;
    } else {
        value.value.integer_value[0] = mixer.muted ? 0 : 1;
        value.value.integer_value[1] = mixer.muted ? 0 : 1;
    }
    return copy_to_user(user_argument, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_elem_write(uint64_t user_argument) {
    struct snd_ctl_elem_value value;
    if (copy_from_user(&value, user_argument, sizeof(value)) != 0) return -EFAULT;
    unsigned numid = element_lookup(&value.id);
    if (!numid || !control_element_count()) return -ENXIO;

    if (numid == CTL_NUMID_VOLUME) {
        long left = value.value.integer_value[0];
        long right = value.value.integer_value[1];
        if (left < 0 || right < 0 || left > (long)card->volume_max ||
            right > (long)card->volume_max) return -EINVAL;
        mixer.left = (uint32_t)left;
        mixer.right = (uint32_t)right;
    } else {
        mixer.muted = !value.value.integer_value[0];
    }
    if (card->set_volume(mixer.left, mixer.right, mixer.muted) != 0) return -EINVAL;
    return 1;
}

static void fill_card_info(struct snd_ctl_card_info *info) {
    memset(info, 0, sizeof(*info));
    info->card = 0;
    copy_field(info->id, sizeof(info->id), card->driver);
    copy_field(info->driver, sizeof(info->driver), card->driver);
    copy_field(info->name, sizeof(info->name), card->name);
    copy_field(info->longname, sizeof(info->longname), card->name);
    copy_field(info->mixername, sizeof(info->mixername), card->mixer_name);
    copy_field(info->components, sizeof(info->components), card->components);
}

int64_t sound_control_ioctl(struct vfs_node *node, unsigned long request,
                            uint64_t user_argument) {
    (void)node;
    if (!card) return -ENXIO;
    if (SND_IOC_TYPE(request) != 'U') return -ENOTTY;

    switch (SND_IOC_NR(request)) {
    case SNDRV_CTL_IOCTL_NR_PVERSION: {
        int version = SNDRV_CTL_VERSION;
        return copy_to_user(user_argument, &version, sizeof(version)) == 0 ? 0 : -EFAULT;
    }
    case SNDRV_CTL_IOCTL_NR_CARD_INFO: {
        struct snd_ctl_card_info info;
        fill_card_info(&info);
        return copy_to_user(user_argument, &info, sizeof(info)) == 0 ? 0 : -EFAULT;
    }
    case SNDRV_CTL_IOCTL_NR_ELEM_LIST:
        return ioctl_elem_list(user_argument);
    case SNDRV_CTL_IOCTL_NR_ELEM_INFO:
        return ioctl_elem_info(user_argument);
    case SNDRV_CTL_IOCTL_NR_ELEM_READ:
        return ioctl_elem_read(user_argument);
    case SNDRV_CTL_IOCTL_NR_ELEM_WRITE:
        return ioctl_elem_write(user_argument);
    case SNDRV_CTL_IOCTL_NR_SUBSCRIBE_EVENTS: {
        int subscribed = 0;
        return copy_to_user(user_argument, &subscribed, sizeof(subscribed)) == 0
                   ? 0 : -EFAULT;
    }
    case SNDRV_CTL_IOCTL_NR_PCM_NEXT_DEVICE: {
        int device = -1;
        if (copy_from_user(&device, user_argument, sizeof(device)) != 0) return -EFAULT;
        /* Device 0 is the only one; anything past it ends the enumeration. */
        device = device < 0 ? 0 : -1;
        return copy_to_user(user_argument, &device, sizeof(device)) == 0 ? 0 : -EFAULT;
    }
    case SNDRV_CTL_IOCTL_NR_PCM_INFO: {
        struct snd_pcm_info info;
        if (copy_from_user(&info, user_argument, sizeof(info)) != 0) return -EFAULT;
        if (info.device != 0 || info.stream != SNDRV_PCM_STREAM_PLAYBACK)
            return -ENXIO;
        fill_pcm_info(&info);
        return copy_to_user(user_argument, &info, sizeof(info)) == 0 ? 0 : -EFAULT;
    }
    case SNDRV_CTL_IOCTL_NR_PCM_PREFER_SUBDEVICE:
        return 0;
    case SNDRV_CTL_IOCTL_NR_POWER_STATE: {
        int state = 0;
        return copy_to_user(user_argument, &state, sizeof(state)) == 0 ? 0 : -EFAULT;
    }
    default:
        return -ENOTTY;
    }
}

/* ------------------------------------------------------------ registration */

int snd_register_card(const struct snd_backend *backend) {
    if (!backend || !backend->configure || !backend->trigger || !backend->position)
        return -1;
    if (ring_allocate() != 0) return -1;
    card = backend;
    memset(&pcm, 0, sizeof(pcm));
    pcm.state = SNDRV_PCM_STATE_OPEN;
    /* Start at full scale: the codec's own zero-decibel setting is already the
       gain the path was opened with. */
    mixer.left = backend->volume_max;
    mixer.right = backend->volume_max;
    mixer.muted = 0;
    return 0;
}

void sound_init(void) {
    if (card) return;
    (void)hda_init();
}

int sound_card_available(void) {
    return card != NULL;
}
