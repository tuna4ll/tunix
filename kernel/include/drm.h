#ifndef TUNIX_DRM_H
#define TUNIX_DRM_H

#include <stddef.h>
#include <stdint.h>

struct file;
struct vfs_node;

/*
 * A DRM/KMS device in the spirit of Linux's simpledrm: the only memory objects
 * are dumb buffers -- plain pages userspace maps and draws into with the CPU.
 * The scanout is the framebuffer the bootloader handed us, unless there is a
 * virtio-gpu, in which case a dumb buffer is scanned out where it lies (see
 * virtgpu.h).
 *
 * This exists because /dev/fb0 and the TUNIX_FBIO_* ioctls are ours alone.
 * Everything in the Linux graphics world -- mesa's GBM, weston's drm backend,
 * kmscube -- talks to /dev/dri/card0 instead. Providing that node is what makes
 * unmodified graphics software able to run here at all.
 *
 * Deliberately absent: GEM sharing between processes, PRIME/dma-buf, atomic
 * modesetting, and any notion of a second CRTC or connector. There is one
 * fixed mode, the one the display is already in.
 */

void drm_init(void);
int drm_available(void);

int64_t drm_node_ioctl(struct vfs_node *node, unsigned long request,
                       uint64_t user_argument);
int64_t drm_file_ioctl(struct file *file, unsigned long request,
                       uint64_t user_argument);
int64_t drm_device_mmap(struct vfs_node *node, struct file *file,
                        uint64_t cr3, uint64_t virtual_address,
                        uint64_t length, uint64_t offset,
                        uint64_t page_flags);
int64_t drm_device_read(struct vfs_node *node, uint64_t offset,
                        size_t size, void *buffer);
int drm_device_read_ready(struct vfs_node *node);
/* Release a reference taken by a PRIME export; called when the descriptor is
   closed, which may be after the buffer's handle is already gone. */
void drm_buffer_put(uint32_t handle);
/* Map a PRIME-exported buffer. The descriptor is the buffer, so the offset is
   an offset into it rather than the fake handle token MAP_DUMB hands out. */
int64_t drm_dmabuf_mmap(struct file *file, uint64_t cr3, uint64_t virtual_address,
                        uint64_t length, uint64_t offset, uint64_t page_flags);
void drm_device_open(struct vfs_node *node);
void drm_device_close(struct vfs_node *node);
void drm_file_close(struct file *file);

/*
 * The virtual terminal the client is on has been switched away from, or back
 * to. Suspending stops presenting and hands a virtio-gpu's scanout back, so the
 * text console has the screen; resuming puts the last frame that was presented
 * back up, because the client has gone on drawing into its own buffer all along
 * and has no reason to think anything changed.
 */
void drm_display_suspend(void);
void drm_display_resume(void);

#endif
