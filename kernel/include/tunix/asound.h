#ifndef TUNIX_ASOUND_H
#define TUNIX_ASOUND_H

#include <stdint.h>

/*
 * ALSA's userspace ABI, the subset Tunix implements.
 *
 * Not a free choice: alsa-lib -- and so PipeWire, whose only audio backend for
 * a PCI card is the ALSA one -- talks to the /dev/snd nodes through exactly
 * these structures and ioctl numbers. Every offset here matches Linux on x86-64,
 * because the size of a structure is encoded in the ioctl number itself.
 *
 * Timestamps are two 64-bit words, which is both what struct timespec is on
 * this architecture and what the newer explicit-width layout uses, so the two
 * variants of every affected structure are the same bytes.
 */

#define SNDRV_PROTOCOL_VERSION(major, minor, subminor) \
    (((major) << 16) | ((minor) << 8) | (subminor))
/* 2.0.13 keeps alsa-lib on the plain STATUS/SYNC_PTR path, which is the one
   this driver can answer without an interrupt of its own. */
#define SNDRV_PCM_VERSION SNDRV_PROTOCOL_VERSION(2, 0, 13)
#define SNDRV_CTL_VERSION SNDRV_PROTOCOL_VERSION(2, 0, 7)

#define SND_IOC_NONE 0U
#define SND_IOC_WRITE 1U
#define SND_IOC_READ 2U
#define SND_IOC(dir, type, nr, size) \
    (((dir) << 30) | ((size) << 16) | ((type) << 8) | (nr))
#define SND_IOC_DIR(request) (((request) >> 30) & 3U)
#define SND_IOC_TYPE(request) (((request) >> 8) & 0xFFU)
#define SND_IOC_NR(request) ((request) & 0xFFU)
#define SND_IOC_SIZE(request) (((request) >> 16) & 0x3FFFU)

struct snd_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* PCM stream direction. */
#define SNDRV_PCM_STREAM_PLAYBACK 0
#define SNDRV_PCM_STREAM_CAPTURE 1

/* Access modes. */
#define SNDRV_PCM_ACCESS_MMAP_INTERLEAVED 0
#define SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED 1
#define SNDRV_PCM_ACCESS_MMAP_COMPLEX 2
#define SNDRV_PCM_ACCESS_RW_INTERLEAVED 3
#define SNDRV_PCM_ACCESS_RW_NONINTERLEAVED 4

/* Sample formats, the few this driver can produce. */
#define SNDRV_PCM_FORMAT_S8 0
#define SNDRV_PCM_FORMAT_U8 1
#define SNDRV_PCM_FORMAT_S16_LE 2
#define SNDRV_PCM_FORMAT_S32_LE 10

#define SNDRV_PCM_SUBFORMAT_STD 0

/* States, in the order the state machine moves through them. */
#define SNDRV_PCM_STATE_OPEN 0
#define SNDRV_PCM_STATE_SETUP 1
#define SNDRV_PCM_STATE_PREPARED 2
#define SNDRV_PCM_STATE_RUNNING 3
#define SNDRV_PCM_STATE_XRUN 4
#define SNDRV_PCM_STATE_DRAINING 5
#define SNDRV_PCM_STATE_PAUSED 6
#define SNDRV_PCM_STATE_SUSPENDED 7
#define SNDRV_PCM_STATE_DISCONNECTED 8

/* What the card can do, as reported in snd_pcm_info and hw_params.info. */
#define SNDRV_PCM_INFO_MMAP 0x00000001U
#define SNDRV_PCM_INFO_MMAP_VALID 0x00000002U
#define SNDRV_PCM_INFO_DOUBLE 0x00000004U
#define SNDRV_PCM_INFO_BATCH 0x00000010U
#define SNDRV_PCM_INFO_INTERLEAVED 0x00000100U
#define SNDRV_PCM_INFO_NONINTERLEAVED 0x00000200U
#define SNDRV_PCM_INFO_BLOCK_TRANSFER 0x00010000U
#define SNDRV_PCM_INFO_OVERRANGE 0x00020000U
#define SNDRV_PCM_INFO_PAUSE 0x00080000U
#define SNDRV_PCM_INFO_HALF_DUPLEX 0x00100000U
#define SNDRV_PCM_INFO_JOINT_DUPLEX 0x00200000U
#define SNDRV_PCM_INFO_SYNC_START 0x00400000U
#define SNDRV_PCM_INFO_NO_PERIOD_WAKEUP 0x00800000U

/* hw_params slots. The first three are masks, the rest intervals. */
#define SNDRV_PCM_HW_PARAM_ACCESS 0
#define SNDRV_PCM_HW_PARAM_FORMAT 1
#define SNDRV_PCM_HW_PARAM_SUBFORMAT 2
#define SNDRV_PCM_HW_PARAM_FIRST_MASK 0
#define SNDRV_PCM_HW_PARAM_LAST_MASK 2
#define SNDRV_PCM_HW_PARAM_SAMPLE_BITS 8
#define SNDRV_PCM_HW_PARAM_FRAME_BITS 9
#define SNDRV_PCM_HW_PARAM_CHANNELS 10
#define SNDRV_PCM_HW_PARAM_RATE 11
#define SNDRV_PCM_HW_PARAM_PERIOD_TIME 12
#define SNDRV_PCM_HW_PARAM_PERIOD_SIZE 13
#define SNDRV_PCM_HW_PARAM_PERIOD_BYTES 14
#define SNDRV_PCM_HW_PARAM_PERIODS 15
#define SNDRV_PCM_HW_PARAM_BUFFER_TIME 16
#define SNDRV_PCM_HW_PARAM_BUFFER_SIZE 17
#define SNDRV_PCM_HW_PARAM_BUFFER_BYTES 18
#define SNDRV_PCM_HW_PARAM_TICK_TIME 19
#define SNDRV_PCM_HW_PARAM_FIRST_INTERVAL 8
#define SNDRV_PCM_HW_PARAM_LAST_INTERVAL 19
#define SNDRV_PCM_HW_PARAM_MASK_COUNT 3
#define SNDRV_PCM_HW_PARAM_INTERVAL_COUNT 12

#define SNDRV_PCM_HW_PARAMS_NORESAMPLE 0x00000001U
#define SNDRV_PCM_HW_PARAMS_EXPORT_BUFFER 0x00000002U
#define SNDRV_PCM_HW_PARAMS_NO_PERIOD_WAKEUP 0x00000004U

#define SNDRV_MASK_BITS 256
#define SNDRV_MASK_WORDS (SNDRV_MASK_BITS / 32)

struct snd_mask {
    uint32_t bits[SNDRV_MASK_WORDS];
};

struct snd_interval {
    unsigned int min;
    unsigned int max;
    unsigned int openmin : 1;
    unsigned int openmax : 1;
    unsigned int integer : 1;
    unsigned int empty : 1;
};

struct snd_pcm_hw_params {
    unsigned int flags;
    struct snd_mask masks[SNDRV_PCM_HW_PARAM_MASK_COUNT];
    struct snd_mask reserved_masks[5];
    struct snd_interval intervals[SNDRV_PCM_HW_PARAM_INTERVAL_COUNT];
    struct snd_interval reserved_intervals[9];
    unsigned int rmask;
    unsigned int cmask;
    unsigned int info;
    unsigned int msbits;
    unsigned int rate_num;
    unsigned int rate_den;
    unsigned long fifo_size;
    unsigned char reserved[64];
};

struct snd_pcm_sw_params {
    int tstamp_mode;
    unsigned int period_step;
    unsigned int sleep_min;
    unsigned long avail_min;
    unsigned long xfer_align;
    unsigned long start_threshold;
    unsigned long stop_threshold;
    unsigned long silence_threshold;
    unsigned long silence_size;
    unsigned long boundary;
    unsigned int proto;
    unsigned int tstamp_type;
    unsigned char reserved[56];
};

struct snd_pcm_info {
    unsigned int device;
    unsigned int subdevice;
    int stream;
    int card;
    unsigned char id[64];
    unsigned char name[80];
    unsigned char subname[32];
    int dev_class;
    int dev_subclass;
    unsigned int subdevices_count;
    unsigned int subdevices_avail;
    unsigned char sync[16];
    unsigned char reserved[64];
};

struct snd_pcm_channel_info {
    unsigned int channel;
    long offset;
    unsigned int first;
    unsigned int step;
};

struct snd_pcm_status {
    int state;
    int pad;
    struct snd_timespec trigger_tstamp;
    struct snd_timespec tstamp;
    unsigned long appl_ptr;
    unsigned long hw_ptr;
    long delay;
    unsigned long avail;
    unsigned long avail_max;
    unsigned long overrange;
    int suspended_state;
    unsigned int audio_tstamp_data;
    struct snd_timespec audio_tstamp;
    struct snd_timespec driver_tstamp;
    unsigned int audio_tstamp_accuracy;
    unsigned char reserved[20];
};

struct snd_pcm_mmap_status {
    int state;
    int pad1;
    unsigned long hw_ptr;
    struct snd_timespec tstamp;
    int suspended_state;
    int pad2;
    struct snd_timespec audio_tstamp;
};

struct snd_pcm_mmap_control {
    unsigned long appl_ptr;
    unsigned long avail_min;
};

#define SNDRV_PCM_SYNC_PTR_HWSYNC 0x00000001U
#define SNDRV_PCM_SYNC_PTR_APPL 0x00000002U
#define SNDRV_PCM_SYNC_PTR_AVAIL_MIN 0x00000004U

struct snd_pcm_sync_ptr {
    unsigned int flags;
    unsigned int pad;
    union {
        struct snd_pcm_mmap_status status;
        unsigned char reserved[64];
    } s;
    union {
        struct snd_pcm_mmap_control control;
        unsigned char reserved[64];
    } c;
};

struct snd_xferi {
    long result;
    uint64_t buf;
    unsigned long frames;
};

struct snd_xfern {
    long result;
    uint64_t bufs;
    unsigned long frames;
};

/* mmap offsets: the data buffer at zero, the two shared pages above it. */
#define SNDRV_PCM_MMAP_OFFSET_DATA 0x00000000UL
#define SNDRV_PCM_MMAP_OFFSET_STATUS 0x80000000UL
#define SNDRV_PCM_MMAP_OFFSET_CONTROL 0x81000000UL

#define SNDRV_PCM_IOCTL_PVERSION SND_IOC(SND_IOC_READ, 'A', 0x00, 4)
#define SNDRV_PCM_IOCTL_INFO SND_IOC(SND_IOC_READ, 'A', 0x01, 288)
#define SNDRV_PCM_IOCTL_TSTAMP SND_IOC(SND_IOC_WRITE, 'A', 0x02, 4)
#define SNDRV_PCM_IOCTL_TTSTAMP SND_IOC(SND_IOC_WRITE, 'A', 0x03, 4)
#define SNDRV_PCM_IOCTL_USER_PVERSION SND_IOC(SND_IOC_WRITE, 'A', 0x04, 4)
#define SNDRV_PCM_IOCTL_NR_HW_REFINE 0x10
#define SNDRV_PCM_IOCTL_NR_HW_PARAMS 0x11
#define SNDRV_PCM_IOCTL_NR_HW_FREE 0x12
#define SNDRV_PCM_IOCTL_NR_SW_PARAMS 0x13
#define SNDRV_PCM_IOCTL_NR_STATUS 0x20
#define SNDRV_PCM_IOCTL_NR_DELAY 0x21
#define SNDRV_PCM_IOCTL_NR_HWSYNC 0x22
#define SNDRV_PCM_IOCTL_NR_SYNC_PTR 0x23
#define SNDRV_PCM_IOCTL_NR_STATUS_EXT 0x24
#define SNDRV_PCM_IOCTL_NR_CHANNEL_INFO 0x32
#define SNDRV_PCM_IOCTL_NR_PREPARE 0x40
#define SNDRV_PCM_IOCTL_NR_RESET 0x41
#define SNDRV_PCM_IOCTL_NR_START 0x42
#define SNDRV_PCM_IOCTL_NR_DROP 0x43
#define SNDRV_PCM_IOCTL_NR_DRAIN 0x44
#define SNDRV_PCM_IOCTL_NR_PAUSE 0x45
#define SNDRV_PCM_IOCTL_NR_REWIND 0x46
#define SNDRV_PCM_IOCTL_NR_RESUME 0x47
#define SNDRV_PCM_IOCTL_NR_XRUN 0x48
#define SNDRV_PCM_IOCTL_NR_FORWARD 0x49
#define SNDRV_PCM_IOCTL_NR_WRITEI_FRAMES 0x50
#define SNDRV_PCM_IOCTL_NR_READI_FRAMES 0x51
#define SNDRV_PCM_IOCTL_NR_WRITEN_FRAMES 0x52
#define SNDRV_PCM_IOCTL_NR_READN_FRAMES 0x53
#define SNDRV_PCM_IOCTL_NR_LINK 0x60
#define SNDRV_PCM_IOCTL_NR_UNLINK 0x61

/* Control device. */
struct snd_ctl_card_info {
    int card;
    int pad;
    unsigned char id[16];
    unsigned char driver[16];
    unsigned char name[32];
    unsigned char longname[80];
    unsigned char reserved[16];
    unsigned char mixername[80];
    unsigned char components[128];
};

#define SNDRV_CTL_ELEM_IFACE_CARD 0
#define SNDRV_CTL_ELEM_IFACE_MIXER 2
#define SNDRV_CTL_ELEM_IFACE_PCM 3

#define SNDRV_CTL_ELEM_TYPE_BOOLEAN 1
#define SNDRV_CTL_ELEM_TYPE_INTEGER 2

#define SNDRV_CTL_ELEM_ACCESS_READ 0x0001U
#define SNDRV_CTL_ELEM_ACCESS_WRITE 0x0002U
#define SNDRV_CTL_ELEM_ACCESS_READWRITE 0x0003U

struct snd_ctl_elem_id {
    unsigned int numid;
    int iface;
    unsigned int device;
    unsigned int subdevice;
    unsigned char name[44];
    unsigned int index;
};

struct snd_ctl_elem_list {
    unsigned int offset;
    unsigned int space;
    unsigned int used;
    unsigned int count;
    uint64_t pids;
    unsigned char reserved[50];
};

struct snd_ctl_elem_info {
    struct snd_ctl_elem_id id;
    int type;
    unsigned int access;
    unsigned int count;
    int owner;
    union {
        struct {
            long min;
            long max;
            long step;
        } integer;
        unsigned char reserved[128];
    } value;
    unsigned short dimen[4];
    unsigned char reserved[56];
};

struct snd_ctl_elem_value {
    struct snd_ctl_elem_id id;
    unsigned int indirect;
    union {
        long integer_value[128];
        unsigned char reserved[1024];
    } value;
    struct snd_timespec tstamp;
    unsigned char reserved[112];
};

#define SNDRV_CTL_IOCTL_NR_PVERSION 0x00
#define SNDRV_CTL_IOCTL_NR_CARD_INFO 0x01
#define SNDRV_CTL_IOCTL_NR_ELEM_LIST 0x10
#define SNDRV_CTL_IOCTL_NR_ELEM_INFO 0x11
#define SNDRV_CTL_IOCTL_NR_ELEM_READ 0x12
#define SNDRV_CTL_IOCTL_NR_ELEM_WRITE 0x13
#define SNDRV_CTL_IOCTL_NR_SUBSCRIBE_EVENTS 0x16
#define SNDRV_CTL_IOCTL_NR_PCM_NEXT_DEVICE 0x30
#define SNDRV_CTL_IOCTL_NR_PCM_INFO 0x31
#define SNDRV_CTL_IOCTL_NR_PCM_PREFER_SUBDEVICE 0x32
#define SNDRV_CTL_IOCTL_NR_POWER_STATE 0xD1

#endif
