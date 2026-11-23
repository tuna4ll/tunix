#ifndef TUNIX_VIRTGPU_H
#define TUNIX_VIRTGPU_H

#include <stdint.h>

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

uint32_t virtgpu_resource_create_3d(const struct virtgpu_resource_3d *spec, const uint64_t *pages,
                                    uint64_t page_count, uint64_t bytes);
int virtgpu_transfer_3d(uint32_t context, uint32_t resource, const struct virtgpu_box *box,
                        uint64_t offset, uint32_t level, uint32_t stride, uint32_t layer_stride,
                        int to_host);
int virtgpu_submit_3d(uint32_t context, const void *buffer, uint32_t bytes);
uint32_t virtgpu_display_width(void);
uint32_t virtgpu_display_height(void);

uint32_t virtgpu_resource_create(uint32_t width, uint32_t height, const uint64_t *pages,
                                 uint64_t page_count);
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

int virtgpu_console_present(uint64_t physical, uint32_t stride_pixels, uint32_t width,
                            uint32_t height);

#define VIRTGPU_FEATURE_BLOB         1
#define VIRTGPU_FEATURE_CONTEXT_INIT 2
#define VIRTGPU_FEATURE_HOST_VISIBLE 4
#define VIRTGPU_CAPSET_VENUS         4

int virtgpu_features(void);
uint32_t virtgpu_capset_mask(void);
int virtgpu_capset_info(uint32_t id, uint32_t *version, uint32_t *size);
int virtgpu_context_create_init(uint32_t context, const char *name, uint32_t capset);
uint32_t virtgpu_resource_create_blob(uint32_t context, uint32_t blob_mem, uint32_t blob_flags,
                                      uint64_t blob_id, uint64_t size, const uint64_t *pages,
                                      uint64_t page_count);
int virtgpu_map_blob(uint32_t resource, uint64_t size, uint64_t *physical, uint32_t *map_info);
void virtgpu_unmap_blob(uint32_t resource);
uint64_t virtgpu_submit_fenced(uint32_t context, uint32_t ring, const void *buffer, uint32_t bytes);
int virtgpu_fence_signaled(uint64_t sequence);

// The virtio_gpu module provides these; until it is loaded, or after it is removed,
// the virtgpu_*() calls above answer as if there were no device.
struct virtgpu_ops {
    int (*available)(void);
    int (*flush_pending)(void);
    int (*cursor_available)(void);
    int (*cursor_set)(uint32_t resource, uint32_t stride_pixels, int upload, int32_t x, int32_t y, uint32_t hot_x, uint32_t hot_y);
    int (*cursor_move)(int32_t x, int32_t y);
    uint64_t (*interrupt_count)(void);
    int (*virgl_available)(void);
    uint32_t (*capset_id)(void);
    uint32_t (*capset_version)(void);
    uint32_t (*capset_size)(void);
    int (*get_capset)(uint32_t id, uint32_t version, void *out, uint32_t bytes);
    int (*pci_identity)(struct virtgpu_pci_identity *out);
    int (*context_create)(uint32_t context, const char *name);
    void (*context_destroy)(uint32_t context);
    int (*context_attach)(uint32_t context, uint32_t resource, int attach);
    uint32_t (*resource_create_3d)(const struct virtgpu_resource_3d *spec, const uint64_t *pages, uint64_t page_count, uint64_t bytes);
    int (*transfer_3d)(uint32_t context, uint32_t resource, const struct virtgpu_box *box, uint64_t offset, uint32_t level, uint32_t stride, uint32_t layer_stride, int to_host);
    int (*submit_3d)(uint32_t context, const void *buffer, uint32_t bytes);
    uint32_t (*display_width)(void);
    uint32_t (*display_height)(void);
    uint32_t (*resource_create)(uint32_t width, uint32_t height, const uint64_t *pages, uint64_t page_count);
    uint64_t (*resource_release)(uint32_t resource);
    int (*sequence_done)(uint64_t sequence);
    int (*wait_sequence)(uint64_t sequence);
    uint64_t (*posted)(void);
    uint64_t (*flip_fence)(void);
    uint64_t (*completed)(void);
    int (*present)(uint32_t resource, uint32_t stride_pixels, uint32_t width, uint32_t height, const struct virtgpu_rect *damage, int upload);
    void (*scanout_disable)(void);
    int (*console_present)(uint64_t physical, uint32_t stride_pixels, uint32_t width, uint32_t height);
    int (*features)(void);
    uint32_t (*capset_mask)(void);
    int (*capset_info)(uint32_t id, uint32_t *version, uint32_t *size);
    int (*context_create_init)(uint32_t context, const char *name, uint32_t capset);
    uint32_t (*resource_create_blob)(uint32_t context, uint32_t blob_mem, uint32_t blob_flags,
                                     uint64_t blob_id, uint64_t size, const uint64_t *pages,
                                     uint64_t page_count);
    int (*map_blob)(uint32_t resource, uint64_t size, uint64_t *physical, uint32_t *map_info);
    void (*unmap_blob)(uint32_t resource);
    uint64_t (*submit_fenced)(uint32_t context, uint32_t ring, const void *buffer, uint32_t bytes);
    int (*fence_signaled)(uint64_t sequence);
};

int virtgpu_register(const struct virtgpu_ops *ops);
void virtgpu_unregister(void);
// Keeps the driver loaded while /dev/dri is open.
int virtgpu_hold(void);
void virtgpu_release(void);

#endif
