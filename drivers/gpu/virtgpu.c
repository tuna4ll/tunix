#include <stddef.h>
#include <stdint.h>

#include <tunix/devfs.h>
#include <tunix/drm.h>
#include <tunix/errno.h>
#include <tunix/module.h>
#include <tunix/sysfs.h>
#include <tunix/virtgpu.h>

// What stands between the DRM device and the virtio_gpu module. Every call takes a
// reference on the module for as long as it runs, so the module cannot be removed
// under it; once removal has started the reference is refused and the call answers
// as if there were no device.

static const struct virtgpu_ops *ops;
static struct module *owner;

static const struct virtgpu_ops *enter(void) {
    const struct virtgpu_ops *driver = __atomic_load_n(&ops, __ATOMIC_ACQUIRE);
    if (!driver || module_get(owner) != 0) return NULL;
    if (__atomic_load_n(&ops, __ATOMIC_ACQUIRE) != driver) {
        module_put(owner);
        return NULL;
    }
    return driver;
}

static void leave(void) { module_put(owner); }

int virtgpu_register(const struct virtgpu_ops *driver) {
    if (!driver || __atomic_load_n(&ops, __ATOMIC_ACQUIRE)) return -EBUSY;
    // A client drawing through the boot framebuffer cannot be moved to the device.
    if (drm_is_open()) return -EBUSY;
    owner = module_active();
    sysfs_remove_drm();
    __atomic_store_n(&ops, driver, __ATOMIC_RELEASE);
    devfs_publish_render_node(virtgpu_virgl_available());
    sysfs_publish_drm();
    return 0;
}

void virtgpu_unregister(void) {
    sysfs_remove_drm();
    devfs_publish_render_node(0);
    __atomic_store_n(&ops, NULL, __ATOMIC_RELEASE);
    owner = NULL;
    sysfs_publish_drm();
}

int virtgpu_hold(void) {
    const struct virtgpu_ops *driver = enter();
    return driver ? 0 : -1;
}

void virtgpu_release(void) { leave(); }

int virtgpu_available(void) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    int result = driver->available();
    leave();
    return result;
}

int virtgpu_flush_pending(void) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    int result = driver->flush_pending();
    leave();
    return result;
}

int virtgpu_cursor_available(void) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    int result = driver->cursor_available();
    leave();
    return result;
}

int virtgpu_cursor_set(uint32_t resource, uint32_t stride_pixels, int upload, int32_t x, int32_t y, uint32_t hot_x, uint32_t hot_y) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return -1;
    int result = driver->cursor_set(resource, stride_pixels, upload, x, y, hot_x, hot_y);
    leave();
    return result;
}

int virtgpu_cursor_move(int32_t x, int32_t y) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return -1;
    int result = driver->cursor_move(x, y);
    leave();
    return result;
}

uint64_t virtgpu_interrupt_count(void) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    uint64_t result = driver->interrupt_count();
    leave();
    return result;
}

int virtgpu_virgl_available(void) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    int result = driver->virgl_available();
    leave();
    return result;
}

uint32_t virtgpu_capset_id(void) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    uint32_t result = driver->capset_id();
    leave();
    return result;
}

uint32_t virtgpu_capset_version(void) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    uint32_t result = driver->capset_version();
    leave();
    return result;
}

uint32_t virtgpu_capset_size(void) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    uint32_t result = driver->capset_size();
    leave();
    return result;
}

int virtgpu_get_capset(uint32_t id, uint32_t version, void *out, uint32_t bytes) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return -1;
    int result = driver->get_capset(id, version, out, bytes);
    leave();
    return result;
}

int virtgpu_pci_identity(struct virtgpu_pci_identity *out) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return -1;
    int result = driver->pci_identity(out);
    leave();
    return result;
}

int virtgpu_context_create(uint32_t context, const char *name) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return -1;
    int result = driver->context_create(context, name);
    leave();
    return result;
}

void virtgpu_context_destroy(uint32_t context) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return;
    driver->context_destroy(context);
    leave();
}

int virtgpu_context_attach(uint32_t context, uint32_t resource, int attach) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return -1;
    int result = driver->context_attach(context, resource, attach);
    leave();
    return result;
}

uint32_t virtgpu_resource_create_3d(const struct virtgpu_resource_3d *spec, const uint64_t *pages, uint64_t page_count, uint64_t bytes) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    uint32_t result = driver->resource_create_3d(spec, pages, page_count, bytes);
    leave();
    return result;
}

int virtgpu_transfer_3d(uint32_t context, uint32_t resource, const struct virtgpu_box *box, uint64_t offset, uint32_t level, uint32_t stride, uint32_t layer_stride, int to_host) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return -1;
    int result = driver->transfer_3d(context, resource, box, offset, level, stride, layer_stride, to_host);
    leave();
    return result;
}

int virtgpu_submit_3d(uint32_t context, const void *buffer, uint32_t bytes) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return -1;
    int result = driver->submit_3d(context, buffer, bytes);
    leave();
    return result;
}

uint32_t virtgpu_display_width(void) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    uint32_t result = driver->display_width();
    leave();
    return result;
}

uint32_t virtgpu_display_height(void) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    uint32_t result = driver->display_height();
    leave();
    return result;
}

uint32_t virtgpu_resource_create(uint32_t width, uint32_t height, const uint64_t *pages, uint64_t page_count) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    uint32_t result = driver->resource_create(width, height, pages, page_count);
    leave();
    return result;
}

uint64_t virtgpu_resource_release(uint32_t resource) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    uint64_t result = driver->resource_release(resource);
    leave();
    return result;
}

int virtgpu_sequence_done(uint64_t sequence) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 1;
    int result = driver->sequence_done(sequence);
    leave();
    return result;
}

int virtgpu_wait_sequence(uint64_t sequence) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    int result = driver->wait_sequence(sequence);
    leave();
    return result;
}

uint64_t virtgpu_posted(void) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    uint64_t result = driver->posted();
    leave();
    return result;
}

uint64_t virtgpu_flip_fence(void) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    uint64_t result = driver->flip_fence();
    leave();
    return result;
}

uint64_t virtgpu_completed(void) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return 0;
    uint64_t result = driver->completed();
    leave();
    return result;
}

int virtgpu_present(uint32_t resource, uint32_t stride_pixels, uint32_t width, uint32_t height, const struct virtgpu_rect *damage, int upload) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return -1;
    int result = driver->present(resource, stride_pixels, width, height, damage, upload);
    leave();
    return result;
}

void virtgpu_scanout_disable(void) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return;
    driver->scanout_disable();
    leave();
}

int virtgpu_console_present(uint64_t physical, uint32_t stride_pixels, uint32_t width, uint32_t height) {
    const struct virtgpu_ops *driver = enter();
    if (!driver) return -1;
    int result = driver->console_present(physical, stride_pixels, width, height);
    leave();
    return result;
}
