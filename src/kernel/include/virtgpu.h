#ifndef TUNIX_VIRTGPU_H
#define TUNIX_VIRTGPU_H

#include <stdint.h>

/*
 * virtio-gpu, 2D.
 *
 * The display this replaces is a blit: drm.c copies a client's dumb buffer into
 * the scanout the bootloader set up, byte by byte, every frame. A virtio-gpu
 * host resource is backed by the dumb buffer's own pages, so presenting stops
 * being a copy and becomes three commands on a queue.
 *
 * 3D (virgl) is deliberately absent. Nothing here negotiates it, so the device
 * comes up in 2D mode and mesa keeps using llvmpipe.
 */

int virtgpu_init(void);
int virtgpu_available(void);
uint32_t virtgpu_display_width(void);
uint32_t virtgpu_display_height(void);

/* A host resource whose backing store is `pages`, which stay owned by the
   caller. Returns the resource id, or 0. */
uint32_t virtgpu_resource_create(uint32_t width, uint32_t height,
                                 const uint64_t *pages, uint64_t page_count);
void virtgpu_resource_destroy(uint32_t resource);
int virtgpu_present(uint32_t resource, uint32_t width, uint32_t height);
/* Hand the scanout back, which is what returns a virtio-vga to its VGA
   framebuffer and lets the text console reappear. */
void virtgpu_scanout_disable(void);

#endif
