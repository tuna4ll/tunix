#ifndef TUNIX_VIRTGPU_H
#define TUNIX_VIRTGPU_H

#include <stdint.h>

/*
 * virtio-gpu.
 *
 * The display this replaces is a blit: drm.c copies a client's dumb buffer into
 * the scanout the bootloader set up, byte by byte, every frame. A virtio-gpu
 * host resource is backed by the dumb buffer's own pages, so presenting stops
 * being a copy and becomes three commands on a queue.
 *
 * VIRGL is negotiated where the host offers it. That does not change any of
 * the above -- the display works the same either way -- but it opens the 3D
 * side of the device, whose first question is what the host is capable of.
 */

int virtgpu_init(void);
int virtgpu_available(void);

/*
 * Whether the host will take 3D commands *and* described what it can do with
 * them. Both halves matter: a device can offer the feature and publish no
 * capset, and 3D on such a host is unusable rather than merely limited.
 */
int virtgpu_virgl_available(void);
uint32_t virtgpu_capset_id(void);
uint32_t virtgpu_capset_version(void);
uint32_t virtgpu_capset_size(void);

/*
 * Copy out the capset, which is virglrenderer's description of the GL it can
 * offer. Nothing in the kernel reads it; mesa does, and the kernel's whole job
 * is to hand it over unaltered. Asking for fewer bytes than the capset holds
 * returns its first `bytes`, which is how a mesa older than the host reads the
 * prefix it understands.
 */
int virtgpu_get_capset(uint32_t id, uint32_t version, void *out, uint32_t bytes);
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
