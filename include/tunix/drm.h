#ifndef TUNIX_DRM_H
#define TUNIX_DRM_H

#include <stddef.h>
#include <stdint.h>

struct file;
struct vfs_node;

void drm_init(void);
int drm_available(void);

int64_t drm_file_ioctl(struct file *file, unsigned long request, uint64_t user_argument);
int64_t drm_device_mmap(struct vfs_node *node, struct file *file, uint64_t cr3,
                        uint64_t virtual_address, uint64_t length, uint64_t offset,
                        uint64_t page_flags);
int64_t drm_device_read(struct vfs_node *node, uint64_t offset, size_t size, void *buffer);
int drm_device_read_ready(struct vfs_node *node);
int64_t drm_file_read(struct file *file, size_t size, void *buffer);
int drm_file_read_ready(struct file *file);
void drm_buffer_put(uint32_t handle);
int64_t drm_dmabuf_size(const struct file *file);
int64_t drm_dmabuf_mmap(struct file *file, uint64_t cr3, uint64_t virtual_address, uint64_t length,
                        uint64_t offset, uint64_t page_flags);
void drm_device_open(struct vfs_node *node);
int drm_is_open(void);
void drm_device_close(struct vfs_node *node);
void drm_file_close(struct file *file);

// A driver of a GPU's rendering interface (nouveau's, say): it names the
// device, takes the ioctls from DRM_COMMAND_BASE up, and is told of clients
// closing and buffers going. Its ioctls run under DRM's lock; one that
// waits on the GPU lets go of it with drm_unlock_for_wait.
struct drm_render_driver {
    const char *name;
    const char *desc;
    int major, minor, patchlevel;
    int64_t (*ioctl)(struct file *file, unsigned nr, unsigned size, uint64_t user_argument);
    void (*file_close)(struct file *file);
    void (*buffer_gone)(uint32_t handle);
    // The PCI function it drives, for sysfs.
    uint8_t pci_bus, pci_slot, pci_function;
    uint16_t pci_vendor, pci_device;
};

void drm_register_render_driver(const struct drm_render_driver *driver);
const struct drm_render_driver *drm_render_driver(void);

// GEM buffers for a render driver: system pages, or a stretch of device
// memory (BAR1, say) the CPU maps write-combined.
struct drm_buffer_view {
    const uint64_t *pages;
    uint64_t page_count;
    uint64_t device_physical;
    uint64_t size;
};
int drm_buffer_new_pages(struct file *file, uint64_t size, uint32_t *handle);
// CPU_MAPPABLE 0: device memory only the device reaches; mmap refuses it.
int drm_buffer_new_device(struct file *file, uint64_t physical, uint64_t size, int cpu_mappable,
                          uint32_t *handle);
int drm_buffer_lookup(struct file *file, uint32_t handle, struct drm_buffer_view *view);
uint64_t drm_buffer_map_offset(uint32_t handle);
void drm_unlock_for_wait(void);
void drm_relock_after_wait(void);

void drm_display_suspend(void);
void drm_console_present(void);
void drm_display_resume(void);

#endif
