#ifndef TUNIX_DRM_H
#define TUNIX_DRM_H

#include <stddef.h>
#include <stdint.h>

struct file;
struct vfs_node;

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
int64_t drm_file_read(struct file *file, size_t size, void *buffer);
int drm_file_read_ready(struct file *file);
void drm_buffer_put(uint32_t handle);
int64_t drm_dmabuf_size(const struct file *file);
int64_t drm_dmabuf_mmap(struct file *file, uint64_t cr3, uint64_t virtual_address,
                        uint64_t length, uint64_t offset, uint64_t page_flags);
void drm_device_open(struct vfs_node *node);
void drm_device_close(struct vfs_node *node);
void drm_file_close(struct file *file);

void drm_display_suspend(void);
void drm_console_present(void);
void drm_display_resume(void);

#endif
