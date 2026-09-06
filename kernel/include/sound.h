#ifndef TUNIX_SOUND_H
#define TUNIX_SOUND_H

#include <stddef.h>
#include <stdint.h>

struct vfs_node;
struct file;

/* Sample formats the core hands a backend. Values are ALSA's. */
#define SND_FORMAT_S16_LE 2
#define SND_FORMAT_S32_LE 10

/* What one card can play. The core turns these into ALSA's hw_params
   constraints, so a backend states its limits once and here only. */
struct snd_hardware {
    uint32_t rate_min;
    uint32_t rate_max;
    /* Rates the hardware locks to, if it is not free-running. Zero count means
       anything in [rate_min, rate_max]. */
    const uint32_t *rates;
    unsigned rate_count;
    uint32_t channels_min;
    uint32_t channels_max;
    uint32_t formats;        /* bit per SND_FORMAT_*, indexed by its value */
    uint32_t period_bytes_min;
    uint32_t period_bytes_max;
    uint32_t periods_min;
    uint32_t periods_max;
    uint32_t buffer_bytes_max;
    uint32_t fifo_size;      /* in frames, reported to userspace as latency */
};

struct snd_stream_format {
    uint32_t rate;
    uint32_t channels;
    uint32_t format;         /* SND_FORMAT_* */
};

/*
 * A playback engine. The core owns the ring buffer and the ALSA state machine;
 * the backend owns the hardware and does four things with it.
 */
struct snd_backend {
    const char *driver;      /* short id, as ALSA's card driver field */
    const char *name;
    const char *mixer_name;
    const char *components;
    struct snd_hardware hardware;

    /* Point the engine at a ring made of `page_count` physical pages and set
       the sample format. Called with the stream stopped. */
    int (*configure)(const struct snd_stream_format *format,
                     const uint64_t *pages, unsigned page_count,
                     uint32_t buffer_bytes);
    /* Rewind the engine to the start of the ring. The core's pointer starts
       from zero on every prepare, so the hardware's has to as well. */
    int (*prepare)(void);
    int (*trigger)(int running);
    /* Byte offset into the ring the hardware is playing from. */
    uint32_t (*position)(void);

    /* Mixer, if the card has one. Volume is 0..volume_max per channel. */
    uint32_t volume_max;
    int (*set_volume)(uint32_t left, uint32_t right, int muted);
};

/* Called by a backend from its probe. The core keeps the pointer. */
int snd_register_card(const struct snd_backend *backend);

/* Probe the audio hardware and build the card. Safe to call with none. */
void sound_init(void);
int sound_card_available(void);

/* /dev/snd/pcmC0D0p */
int64_t sound_pcm_ioctl(struct vfs_node *node, unsigned long request,
                        uint64_t user_argument);
int64_t sound_pcm_write(struct vfs_node *node, uint64_t offset, size_t size,
                        const void *buffer);
int64_t sound_pcm_mmap(struct vfs_node *node, struct file *file, uint64_t cr3,
                       uint64_t virtual_address, uint64_t length,
                       uint64_t offset, uint64_t page_flags);
int sound_pcm_write_ready(struct vfs_node *node);
void sound_pcm_open(struct vfs_node *node);
void sound_pcm_close(struct vfs_node *node);

/* /dev/snd/controlC0 */
int64_t sound_control_ioctl(struct vfs_node *node, unsigned long request,
                            uint64_t user_argument);

#endif
