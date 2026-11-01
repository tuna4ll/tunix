#ifndef TUNIX_SOUND_H
#define TUNIX_SOUND_H

#include <stddef.h>
#include <stdint.h>

struct vfs_node;
struct file;

#define SND_FORMAT_S16_LE 2
#define SND_FORMAT_S32_LE 10

#define SND_PLAYBACK 0
#define SND_CAPTURE  1

#define SND_TRIGGER_STOP    0
#define SND_TRIGGER_START   1
#define SND_TRIGGER_PAUSE   2
#define SND_TRIGGER_RELEASE 3

#define SND_MAX_CARDS 4
#define SND_MAX_RATES 16

struct snd_hardware {
    uint32_t rates[SND_MAX_RATES];
    unsigned rate_count;
    uint32_t channels_min;
    uint32_t channels_max;
    uint32_t formats;
    uint32_t msbits_s32;
    uint32_t period_bytes_min;
    uint32_t period_bytes_max;
    uint32_t periods_min;
    uint32_t periods_max;
    uint32_t buffer_bytes_max;
    uint32_t fifo_frames;
};

struct snd_stream_format {
    uint32_t rate;
    uint32_t channels;
    uint32_t format;
    uint32_t msbits;
};

struct snd_pcm_ops {
    struct snd_hardware hardware;
    int (*configure)(void *context, const struct snd_stream_format *format, const uint64_t *pages,
                     unsigned page_count, uint32_t buffer_bytes, uint32_t period_bytes);
    int (*prepare)(void *context);
    int (*trigger)(void *context, int command);
    uint32_t (*position)(void *context);
    void (*release)(void *context);
};

#define SND_CONTROL_BOOLEAN    1
#define SND_CONTROL_INTEGER    2
#define SND_CONTROL_ENUMERATED 3

#define SND_IFACE_CARD  0
#define SND_IFACE_MIXER 2

#define SND_CONTROL_READ     0x0001U
#define SND_CONTROL_WRITE    0x0002U
#define SND_CONTROL_VOLATILE 0x0004U

#define SND_EVENT_VALUE 0x1U
#define SND_EVENT_INFO  0x2U

struct snd_control {
    const char *name;
    uint32_t iface;
    uint32_t index;
    uint32_t type;
    uint32_t count;
    uint32_t access;
    int32_t min;
    int32_t max;
    int32_t step;
    const char *const *items;
    uint32_t item_count;
    int32_t db_min;
    int32_t db_step;
    int db_mute;
};

struct snd_backend {
    const char *driver;
    const char *name;
    const char *longname;
    const char *mixer_name;
    const char *components;
    void *context;
    const struct snd_pcm_ops *streams[2];
    const struct snd_control *controls;
    unsigned control_count;
    int (*control_get)(void *context, unsigned control, long *values);
    int (*control_put)(void *context, unsigned control, const long *values);
    void (*poll)(void *context);
    size_t (*proc_codec)(void *context, char *out, size_t capacity);
};

int snd_register_card(const struct snd_backend *backend);
void snd_unregister_card(const struct snd_backend *backend);
void snd_pcm_period_elapsed(const struct snd_backend *backend, int direction);
void snd_ctl_notify(const struct snd_backend *backend, unsigned control, unsigned mask);
int sound_card_available(void);

const struct vfs_file_ops *sound_control_file_ops(void);
const struct vfs_file_ops *sound_pcm_file_ops(void);
const struct vfs_file_ops *sound_timer_file_ops(void);
int64_t sound_pcm_mmap(struct vfs_node *node, struct file *file, uint64_t cr3,
                       uint64_t virtual_address, uint64_t length, uint64_t offset,
                       uint64_t page_flags);

int sound_card_info(unsigned card, int *playback, int *capture);
size_t sound_proc_cards(char *out, size_t capacity);
size_t sound_proc_devices(char *out, size_t capacity);
size_t sound_proc_pcm(char *out, size_t capacity);
size_t sound_proc_card_file(unsigned card, const char *path, char *out, size_t capacity);

void sound_tick(void);

#endif
