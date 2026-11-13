#include "ext4.h"

#include <tunix/kstring.h>
#include <tunix/process.h>
#include <tunix/random.h>
#include <tunix/time.h>

#define MMP_SEQUENCE_AT  4U
#define MMP_TIME_AT      8U
#define MMP_NODE_AT      16U
#define MMP_DEVICE_AT    80U
#define MMP_INTERVAL_AT  112U
#define MMP_CHECKSUM_AT  1020U
#define MMP_MIN_INTERVAL 5U

static uint8_t mmp_buf[EXT4_MAX_BLOCK_SIZE] __attribute__((aligned(16)));

static uint32_t interval(void) {
    uint32_t seconds = fs->sb.s_mmp_update_interval;
    return seconds < MMP_MIN_INTERVAL ? MMP_MIN_INTERVAL : seconds;
}

static int mmp_read(void) {
    if (ext4_volume_read(fs, fs->mmp_block, 1, mmp_buf) != 0) return -1;
    if (get32(mmp_buf) != EXT4_MMP_MAGIC) return -1;
    if (has_csum() &&
        ext4_csum(fs->csum_seed, mmp_buf, MMP_CHECKSUM_AT) != get32(mmp_buf + MMP_CHECKSUM_AT))
        return -1;
    return 0;
}

static int mmp_write(uint32_t sequence) {
    put32(mmp_buf, EXT4_MMP_MAGIC);
    put32(mmp_buf + MMP_SEQUENCE_AT, sequence);
    uint64_t now = ext4_now();
    memcpy(mmp_buf + MMP_TIME_AT, &now, sizeof(now));
    memset(mmp_buf + MMP_NODE_AT, 0, 64);
    memcpy(mmp_buf + MMP_NODE_AT, "tunix", 5);
    memset(mmp_buf + MMP_DEVICE_AT, 0, 32);
    size_t length = strlen(fs->device->dev_name);
    if (length > 31) length = 31;
    memcpy(mmp_buf + MMP_DEVICE_AT, fs->device->dev_name, length);
    put16(mmp_buf + MMP_INTERVAL_AT, (uint16_t)interval());
    if (has_csum())
        put32(mmp_buf + MMP_CHECKSUM_AT, ext4_csum(fs->csum_seed, mmp_buf, MMP_CHECKSUM_AT));
    if (ext4_volume_write(fs, fs->mmp_block, 1, mmp_buf) != 0) return -1;
    return ext4_volume_flush(fs);
}

static void pause_seconds(uint32_t seconds) {
    uint64_t until = time_uptime_ns() + (uint64_t)seconds * 1000000000ULL;
    while (time_uptime_ns() < until) {
        if (process_may_sleep()) {
            process_prepare_wait(&mmp_buf, until);
            process_wait();
            process_finish_wait();
        }
    }
}

static uint32_t fresh_sequence(void) {
    uint32_t value;
    random_get_bytes(&value, sizeof(value));
    return value % EXT4_MMP_SEQ_MAX + 1U;
}

int mmp_start(void) {
    fs->mmp_block = 0;
    if (!has_incompat(EXT4_INCOMPAT_MMP)) return 0;
    uint64_t block = fs->sb.s_mmp_block;
    if (block <= fs->first_data_block || block >= fs->blocks_count) return -1;
    fs->mmp_block = block;
    if (mmp_read() != 0) {
        kprintf("EXT4: %s has a damaged MMP block\n", fs->device->dev_name);
        fs->mmp_block = 0;
        return -1;
    }
    uint32_t seen = get32(mmp_buf + MMP_SEQUENCE_AT);
    if (seen == EXT4_MMP_SEQ_FSCK) {
        kprintf("EXT4: %s is being checked by e2fsck\n", fs->device->dev_name);
        fs->mmp_block = 0;
        return -1;
    }
    uint32_t wait = 2U * interval() + 1U;
    if (seen != EXT4_MMP_SEQ_CLEAN) {
        kprintf("EXT4: %s may be in use elsewhere, waiting %u seconds\n", fs->device->dev_name,
                wait);
        pause_seconds(wait);
        if (mmp_read() != 0 || get32(mmp_buf + MMP_SEQUENCE_AT) != seen) {
            kprintf("EXT4: %s is in use by another system\n", fs->device->dev_name);
            fs->mmp_block = 0;
            return -1;
        }
    }
    uint32_t ours = fresh_sequence();
    if (mmp_write(ours) != 0) return -1;
    pause_seconds(wait);
    if (mmp_read() != 0 || get32(mmp_buf + MMP_SEQUENCE_AT) != ours) {
        kprintf("EXT4: %s was claimed by another system\n", fs->device->dev_name);
        fs->mmp_block = 0;
        return -1;
    }
    fs->mmp_sequence = fresh_sequence();
    fs->mmp_last_ns = time_uptime_ns();
    return mmp_write(fs->mmp_sequence);
}

void mmp_tick(void) {
    if (!fs->mmp_block) return;
    uint64_t now = time_uptime_ns();
    if (now - fs->mmp_last_ns < (uint64_t)interval() * 1000000000ULL) return;
    fs->mmp_last_ns = now;
    fs->mmp_sequence = fs->mmp_sequence + 1U > EXT4_MMP_SEQ_MAX ? 1U : fs->mmp_sequence + 1U;
    (void)mmp_write(fs->mmp_sequence);
}

void mmp_stop(void) {
    if (!fs->mmp_block) return;
    (void)mmp_write(EXT4_MMP_SEQ_CLEAN);
    fs->mmp_block = 0;
}
