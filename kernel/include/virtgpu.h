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

/*
 * Where the card is on the PCI bus, and what it says it is.
 *
 * Nothing in the kernel needs this -- it drives the device through its
 * capabilities rather than its address. It is published in sysfs, because
 * libdrm identifies a card by its bus address and will not pair the card node
 * with the render node without one.
 */
struct virtgpu_pci_identity {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor;
    uint16_t device;
};
int virtgpu_pci_identity(struct virtgpu_pci_identity *out);

/*
 * A region of a resource. For an image these are pixels; for a buffer only x
 * and w are used and they are bytes.
 */
struct virtgpu_box {
    uint32_t x, y, z;
    uint32_t w, h, d;
};

/*
 * What a 3D resource is to be. Every field is mesa's -- the target, the
 * format and the bind flags are virglrenderer's own enumerations, and the
 * kernel passes them through without knowing what any particular value means.
 */
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

/* A context is one client's view of the host renderer. `name` shows up in the
   host's log and is the only way to tell two of them apart there. */
int virtgpu_context_create(uint32_t context, const char *name);
void virtgpu_context_destroy(uint32_t context);
/* Grant or withdraw a context's right to name a resource. A command buffer
   that mentions one it was not given is refused by the host. */
int virtgpu_context_attach(uint32_t context, uint32_t resource, int attach);

/*
 * `pages` may be NULL, for a resource that lives only on the host and is never
 * read or written by the guest. `bytes` is the resource's own size, which is
 * smaller than the pages holding it whenever it does not end on a page
 * boundary -- and describing the whole of the last page instead makes the host
 * refuse every transfer to or from the resource.
 *
 * Returns the resource id, or 0.
 */
uint32_t virtgpu_resource_create_3d(const struct virtgpu_resource_3d *spec,
                                    const uint64_t *pages, uint64_t page_count,
                                    uint64_t bytes);
int virtgpu_transfer_3d(uint32_t context, uint32_t resource,
                        const struct virtgpu_box *box, uint64_t offset,
                        uint32_t level, uint32_t stride, uint32_t layer_stride,
                        int to_host);
/* Execute a command buffer. This is the one that renders. */
int virtgpu_submit_3d(uint32_t context, const void *buffer, uint32_t bytes);
uint32_t virtgpu_display_width(void);
uint32_t virtgpu_display_height(void);

/* A host resource whose backing store is `pages`, which stay owned by the
   caller. Returns the resource id, or 0. */
uint32_t virtgpu_resource_create(uint32_t width, uint32_t height,
                                 const uint64_t *pages, uint64_t page_count);
void virtgpu_resource_destroy(uint32_t resource);
/* `upload` sends the guest's pages to the host first, which is right for a
   resource the CPU drew into and wrong for one the host rendered itself. */
int virtgpu_present(uint32_t resource, uint32_t width, uint32_t height,
                    int upload);
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
