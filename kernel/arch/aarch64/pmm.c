#include <stddef.h>
#include <stdint.h>

#include "arch.h"

#define PAGE_SIZE 4096UL
#define MAX_FRAMES (2UL * 1024 * 1024)          // covers 8 GiB of RAM

extern char kernel_start[];
extern char __image_end[];

static uint8_t frame_bitmap[MAX_FRAMES / 8];
static uint64_t base_pa;
static uint64_t frame_count;
static uint64_t free_frames;

static void mark_used(uint64_t frame) {
    frame_bitmap[frame >> 3] |= (uint8_t)(1U << (frame & 7));
}

static int is_used(uint64_t frame) {
    return frame_bitmap[frame >> 3] & (1U << (frame & 7));
}

static void reserve_range(uint64_t start, uint64_t end) {
    if (end <= base_pa) return;
    if (start < base_pa) start = base_pa;
    uint64_t first = (start - base_pa) / PAGE_SIZE;
    uint64_t last = (end - base_pa + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t f = first; f < last && f < frame_count; f++)
        if (!is_used(f)) {
            mark_used(f);
            if (free_frames) free_frames--;
        }
}

void pmm_reserve(uint64_t start, uint64_t bytes) {
    reserve_range(start, start + bytes);
}

void pmm_init(uint64_t ram_base, uint64_t ram_bytes, uint64_t dtb, uint64_t dtb_size) {
    base_pa = ram_base;
    frame_count = ram_bytes / PAGE_SIZE;
    if (frame_count > MAX_FRAMES) frame_count = MAX_FRAMES;
    free_frames = frame_count;

    reserve_range((uint64_t)kernel_start, (uint64_t)__image_end);
    if (dtb) reserve_range(dtb, dtb + dtb_size);
}

void *pmm_alloc_page(void) {
    for (uint64_t f = 0; f < frame_count; f++) {
        if (is_used(f)) continue;
        mark_used(f);
        free_frames--;
        uint64_t pa = base_pa + f * PAGE_SIZE;
        uint64_t *p = (uint64_t *)pa;            // identity-mapped, zero it
        for (int i = 0; i < 512; i++) p[i] = 0;
        return (void *)pa;
    }
    return NULL;
}

void pmm_free_page(void *pa) {
    uint64_t frame = ((uint64_t)pa - base_pa) / PAGE_SIZE;
    if (frame < frame_count && is_used(frame)) {
        frame_bitmap[frame >> 3] &= (uint8_t)~(1U << (frame & 7));
        free_frames++;
    }
}

uint64_t pmm_free_pages(void) {
    return free_frames;
}
