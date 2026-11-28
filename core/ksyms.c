#include <stdint.h>

#include <tunix/dma.h>
#include <tunix/heap.h>
#include <tunix/irq.h>
#include <tunix/kstring.h>
#include <tunix/lock.h>
#include <tunix/module.h>
#if defined(__x86_64__)
#include <tunix/acpi.h>
#include <tunix/apic.h>
#include <uacpi/event.h>
#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/status.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include "../drivers/acpi/priv.h"
#endif
#include <tunix/pci.h>
#include <tunix/sound.h>
#include <tunix/pmm.h>
#include <tunix/net/net.h>
#include <tunix/time.h>
#include <tunix/vmm.h>
#include <tunix/kprintf.h>
#include <tunix/timer.h>
#include <tunix/eventfd.h>
#include <tunix/process_arch.h>
#include <tunix/smp.h>
#include <tunix/file.h>
#include <tunix/usercopy.h>
#include <tunix/usb.h>
#include <tunix/workqueue.h>
#include <tunix/virtgpu.h>
#include <tunix/sysfs.h>
#include <tunix/process.h>
#include <tunix/power.h>
#include <tunix/input.h>
#include <tunix/cpufreq.h>
#include <tunix/thermal.h>
#include <tunix/backlight.h>
#include <tunix/virtio.h>
#include <tunix/vfs.h>
#include <tunix/lz4.h>
#include <tunix/swap.h>
#include <tunix/mutex.h>
#include <tunix/devfs.h>
#include <tunix/block.h>
#include <tunix/boot.h>
#include <tunix/display.h>
#include <tunix/drm.h>

extern uint64_t __stack_chk_guard;
extern void __stack_chk_fail(void);

#define EXPORT(symbol) {#symbol, (uint64_t)(uintptr_t)&symbol}

const struct module_export kernel_symbols[] = {
    EXPORT(kprintf),
    EXPORT(boot_command_line_flag),
#if defined(__x86_64__)
    // The nv50 module's: the console it takes, DRM's buffers for nouveau's interface.
    EXPORT(display_early_register),
    EXPORT(display_register_flipper),
    EXPORT(drm_buffer_lookup),
    EXPORT(drm_buffer_map_offset),
    EXPORT(drm_buffer_new_device),
    EXPORT(drm_buffer_new_pages),
    EXPORT(drm_register_render_driver),
    EXPORT(drm_relock_after_wait),
    EXPORT(drm_unlock_for_wait),
    EXPORT(smp_service_flush),
    EXPORT(sysfs_publish_backlight),
    EXPORT(vmm_map_device_write_combining),
#endif
    EXPORT(__stack_chk_guard),
    EXPORT(__stack_chk_fail),
    EXPORT(panic),
    EXPORT(kmalloc),
    EXPORT(kfree),
    EXPORT(memset),
    EXPORT(memcpy),
    EXPORT(memmove),
    EXPORT(strlen),
    EXPORT(strcmp),
    EXPORT(strncmp),
    EXPORT(strncpy),
    EXPORT(dma_alloc),
    EXPORT(dma_alloc_below),
    EXPORT(dma_free),
    EXPORT(pmm_alloc_page),
    EXPORT(pmm_alloc_pages),
    EXPORT(pmm_free_page),
    EXPORT(pmm_free_pages),
    EXPORT(vmm_phys_to_virt),
    EXPORT(vmm_virt_to_phys_direct),
    EXPORT(vmm_dma_physical),
    EXPORT(vmm_map_device),
    EXPORT(vmm_map_page_in),
    EXPORT(vmm_kernel_cr3),
    EXPORT(pci_config_read32),
    EXPORT(pci_config_write32),
    EXPORT(pci_find_device),
    EXPORT(pci_find_class),
    EXPORT(pci_find_nth_class),
    EXPORT(pci_for_each_device),
    EXPORT(pci_enable_bus_mastering),
    EXPORT(pci_bar_address),
    EXPORT(pci_find_capability),
    EXPORT(pci_msix_enable),
    EXPORT(pci_msix_bind),
    EXPORT(pci_msi_bind),
    EXPORT(irq_request),
    EXPORT(irq_release),
    EXPORT(net_register_adapter),
    EXPORT(net_unregister_adapter),
    EXPORT(time_uptime_ns),
    EXPORT(time_realtime_ns),
    EXPORT(time_tsc_frequency),
    EXPORT(module_get),
    EXPORT(module_put),
    EXPORT(module_active),
    EXPORT(pci_register_driver),
    EXPORT(pci_unregister_driver),
    EXPORT(pci_device_driver),
    EXPORT(snd_register_card),
    EXPORT(snd_unregister_card),
    EXPORT(snd_pcm_period_elapsed),
    EXPORT(snd_ctl_notify),
    EXPORT(lock_init),
    EXPORT(lock_acquire),
    EXPORT(lock_release),
    // zram
    EXPORT(block_register_named),
    EXPORT(block_set_sectors),
    EXPORT(devfs_add_block),
    EXPORT(memcmp),
    EXPORT(mutex_lock),
    EXPORT(mutex_unlock),
    EXPORT(pmm_usable_page_count),
    EXPORT(swap_block_in_use),
    EXPORT(tunix_lz4_compress),
    EXPORT(tunix_lz4_decompress),
    EXPORT(vfs_alloc_node),
    EXPORT(vfs_attach),
    EXPORT(vfs_create_symlink),
    EXPORT(vfs_free_node),
    EXPORT(vfs_lookup),
    EXPORT(vfs_mkdir_p),
    // virtio transport, for virtio drivers
    EXPORT(virtio_pci_attach),
    EXPORT(virtio_pci_attach_device),
    EXPORT(virtio_pci_reset),
    EXPORT(virtio_pci_setup_queue),
    EXPORT(virtio_pci_set_driver_ok),
    EXPORT(virtio_pci_request_irq),
    EXPORT(virtio_pci_set_failed),
    EXPORT(virtio_config_read32),
    EXPORT(virtio_queue_submit),
    EXPORT(virtio_queue_post),
    EXPORT(virtio_queue_reclaim),
    EXPORT(virtio_queue_take_used),
    EXPORT(virtio_queue_drain),
    EXPORT(virtio_queue_outstanding),
    EXPORT(virtio_ring_alloc),
    EXPORT(virtio_ring_free),
    // ACPI drivers' use of the rest of the kernel
    EXPORT(backlight_step),
    EXPORT(thermal_set_passive),
    EXPORT(cpufreq_set_limit_khz),
    EXPORT(input_report_hotkey),
    EXPORT(power_critical),
    EXPORT(power_button_pressed),
    EXPORT(process_create_kthread),
    EXPORT(process_prepare_wait),
    EXPORT(process_wait),
    EXPORT(process_finish_wait),
    EXPORT(process_wake_all),
    EXPORT(sysfs_publish_thermal_zone),
    // virtio_gpu
    EXPORT(process_io_recheck),
    EXPORT(process_may_sleep),
    EXPORT(process_wake_io),
    EXPORT(virtgpu_register),
    EXPORT(virtgpu_unregister),
    EXPORT(work_flush),
    EXPORT(work_queue),
    // usbhid
    EXPORT(input_external_key),
    EXPORT(input_external_mouse),
    EXPORT(input_external_tablet),
    EXPORT(input_usb_pointer),
    EXPORT(usb_register_driver),
    EXPORT(usb_unregister_driver),
    // KVM: user memory, files, vCPU threads and interrupts
    EXPORT(copy_from_user),
    EXPORT(copy_to_user),
    EXPORT(pmm_page_ref),
    EXPORT(file_open_node),
    EXPORT(file_ref),
    EXPORT(file_unref),
    EXPORT(file_write),
    EXPORT(process_current),
    EXPORT(process_install_file_flags),
    EXPORT(process_file_get),
    EXPORT(process_preempt_point),
    EXPORT(process_signal_interrupts_wait),
    EXPORT(process_kernel_yield),
    EXPORT(process_pin_user_page),
    EXPORT(mutex_init),
    EXPORT(smp_cpu_count),
    EXPORT(vmm_unmap_page_in),
    EXPORT(eventfd_set_consumer),
    EXPORT(timer_note_deadline),
    EXPORT(smp_send_reschedule_to),
    EXPORT(process_prepare_wait_interruptible),
#if defined(__x86_64__)
    EXPORT(apic_route_global),
    EXPORT(arch_fpu_save),
    EXPORT(arch_fpu_restore),
    EXPORT(arch_fpu_init),
    // ACPI, for the button, processor, thermal_zone and video modules
    EXPORT(acpi_device_present),
    EXPORT(acpi_node_path),
    EXPORT(acpi_subsystem_ready),
    EXPORT(acpi_thermal_zone_count),
    EXPORT(acpi_video_output_count),
    EXPORT(uacpi_eval_simple_integer),
    EXPORT(uacpi_eval_simple_package),
    EXPORT(uacpi_execute),
    EXPORT(uacpi_find_devices),
    EXPORT(uacpi_install_fixed_event_handler),
    EXPORT(uacpi_install_notify_handler),
    EXPORT(uacpi_namespace_for_each_child),
    EXPORT(uacpi_namespace_node_find),
    EXPORT(uacpi_namespace_node_parent),
    EXPORT(uacpi_namespace_root),
    EXPORT(uacpi_object_create_buffer),
    EXPORT(uacpi_object_create_integer),
    EXPORT(uacpi_object_get_integer),
    EXPORT(uacpi_object_get_package),
    EXPORT(uacpi_object_unref),
    EXPORT(uacpi_status_to_string),
    EXPORT(uacpi_uninstall_fixed_event_handler),
    EXPORT(uacpi_uninstall_notify_handler),
#endif
};

const unsigned kernel_symbol_count = (unsigned)(sizeof(kernel_symbols) / sizeof(kernel_symbols[0]));
