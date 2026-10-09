#ifndef TUNIX_VIRTGPU_H
#define TUNIX_VIRTGPU_H

#include <stdint.h>

int virtgpu_init(void);
int virtgpu_available(void);
int virtgpu_flush_pending(void);
int virtgpu_cursor_available(void);
int virtgpu_cursor_set(uint32_t resource, uint32_t stride_pixels, int upload, int32_t x, int32_t y,
                       uint32_t hot_x, uint32_t hot_y);
int virtgpu_cursor_move(int32_t x, int32_t y);
uint64_t virtgpu_interrupt_count(void);

int virtgpu_virgl_available(void);
uint32_t virtgpu_capset_id(void);
uint32_t virtgpu_capset_version(void);
uint32_t virtgpu_capset_size(void);

int virtgpu_get_capset(uint32_t id, uint32_t version, void *out, uint32_t bytes);

struct virtgpu_pci_identity {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor;
    uint16_t device;
};
int virtgpu_pci_identity(struct virtgpu_pci_identity *out);

struct virtgpu_box {
    uint32_t x, y, z;
    uint32_t w, h, d;
};

struct virtgpu_resource_3d {
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
};

int virtgpu_context_create(uint32_t context, const char *name);
void virtgpu_context_destroy(uint32_t context);
int virtgpu_context_attach(uint32_t context, uint32_t resource, int attach);

uint32_t virtgpu_resource_create_3d(const struct virtgpu_resource_3d *spec,
                                    const uint64_t *pages, uint64_t page_count,
                                    uint64_t bytes);
int virtgpu_transfer_3d(uint32_t context, uint32_t resource,
                        const struct virtgpu_box *box, uint64_t offset,
                        uint32_t level, uint32_t stride, uint32_t layer_stride,
                        int to_host);
int virtgpu_submit_3d(uint32_t context, const void *buffer, uint32_t bytes);
uint32_t virtgpu_display_width(void);
uint32_t virtgpu_display_height(void);

uint32_t virtgpu_resource_create(uint32_t width, uint32_t height,
                                 const uint64_t *pages, uint64_t page_count);
uint64_t virtgpu_resource_release(uint32_t resource);
int virtgpu_sequence_done(uint64_t sequence);
int virtgpu_wait_sequence(uint64_t sequence);
uint64_t virtgpu_posted(void);
uint64_t virtgpu_flip_fence(void);
uint64_t virtgpu_completed(void);
struct virtgpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

int virtgpu_present(uint32_t resource, uint32_t stride_pixels, uint32_t width, uint32_t height,
                    const struct virtgpu_rect *damage, int upload);
void virtgpu_scanout_disable(void);

int virtgpu_console_present(uint64_t physical, uint32_t stride_pixels,
                            uint32_t width, uint32_t height);

#endif
