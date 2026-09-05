#ifndef TUNIX_DRM_H
#define TUNIX_DRM_H

#include <stddef.h>
#include <stdint.h>

struct file;
struct vfs_node;

/* A DRM/KMS device in the spirit of simpledrm: dumb buffers, and the
   scanout the bootloader left. */

void drm_init(void);
int drm_available(void);

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

/* Release a reference a PRIME export took, which the handle may already be gone by. */
void drm_display_suspend(void);
/* Scan the text console out, for as long as it is the thing in front. */
void drm_console_present(void);
void drm_display_resume(void);

#endif
