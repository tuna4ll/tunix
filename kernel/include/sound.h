#ifndef TUNIX_SOUND_H
#define TUNIX_SOUND_H

#include <stddef.h>
#include <stdint.h>

struct vfs_node;
struct file;

#define SND_FORMAT_S16_LE 2
#define SND_FORMAT_S32_LE 10

struct snd_hardware {
    uint32_t rate_min;
    uint32_t rate_max;
    const uint32_t *rates;
    unsigned rate_count;
    uint32_t channels_min;
    uint32_t channels_max;
    uint32_t formats;
    uint32_t period_bytes_min;
    uint32_t period_bytes_max;
    uint32_t periods_min;
    uint32_t periods_max;
    uint32_t buffer_bytes_max;
    uint32_t fifo_size;
};

struct snd_stream_format {
    uint32_t rate;
    uint32_t channels;
    uint32_t format;
};

struct snd_backend {
    const char *driver;
    const char *name;
    const char *mixer_name;
    const char *components;
    struct snd_hardware hardware;

    int (*configure)(const struct snd_stream_format *format,
                     const uint64_t *pages, unsigned page_count,
                     uint32_t buffer_bytes);
    int (*prepare)(void);
    int (*trigger)(int running);
    uint32_t (*position)(void);

    uint32_t volume_max;
    int (*set_volume)(uint32_t left, uint32_t right, int muted);
};

int snd_register_card(const struct snd_backend *backend);
void snd_unregister_card(const struct snd_backend *backend);
int sound_card_available(void);

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

int64_t sound_control_ioctl(struct vfs_node *node, unsigned long request,
                            uint64_t user_argument);

void sound_tick(void);

#endif
