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
void virtgpu_scanout_disable(void);

/*
 * Scan out the text console's framebuffer.
 *
 * Handing the display back cannot mean setting the scanout to nothing: a
 * virtio-vga that has ever been given a scanout keeps showing the virtio
 * display, and an empty one reads "Display output is not active" rather than
 * falling back to the VGA framebuffer underneath -- which is where the console
 * draws. So the console gets a resource over those same pages, and giving the
 * display back means scanning that out instead.
 *
 * `physical` is where the framebuffer starts, `stride_pixels` its pitch in
 * pixels, and `width`/`height` the part of it that is the screen.
 */
int virtgpu_console_present(uint64_t physical, uint32_t stride_pixels,
                            uint32_t width, uint32_t height);

#endif
