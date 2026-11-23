#ifndef TUNIX_USB_H
#define TUNIX_USB_H

#include <stdint.h>

struct usb_host {
    const char *name;
    int (*storage_count)(void);
    int (*bulk_transfer)(int index, int in, uint64_t physical, uint32_t length);
    int (*reset_recovery)(int index);
    int (*present)(int index);
    uint32_t max_transfer;
};

void usb_register_host(const struct usb_host *host);
void usb_host_storage_added(const struct usb_host *host, int local_index);

int usb_storage_count(void);
int usb_bulk_transfer(int index, int in, uint64_t physical, uint32_t length);
int usb_reset_recovery(int index);
int usb_storage_present(int index);
uint32_t usb_max_transfer(int index);

// ---- The device model: devices the host controllers found, and class drivers ----
//
// A host controller driver enumerates a device, sets its configuration and hands it
// to usb_device_added() with what its descriptors say. The device is published in
// /sys/bus/usb/devices with Linux's modaliases, so that modprobe can load a driver for
// it, and each interface is offered to the registered drivers. A driver talks to the
// device only through the host's operations, from its probe, its disconnect or a report
// callback, all of which run with the host locked.

#define USB_INTERFACES_MAX 8U
#define USB_ANY            0xFFFFU

struct usb_interface_info {
    uint8_t number;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    uint16_t hid_report_length;     // from a HID descriptor, 0 if there is none
    uint8_t interrupt_in;           // the first interrupt IN endpoint's address, 0 if none
    uint16_t interrupt_packet;
    uint8_t interrupt_interval;
};

// Called with a report from an interrupt endpoint; may run in interrupt context.
typedef void (*usb_report_fn)(void *context, const uint8_t *data, uint32_t length);

struct usb_host_ops {
    int (*control)(void *device, uint8_t request_type, uint8_t request, uint16_t value,
                   uint16_t index, void *data, uint16_t length);
    // Polls INTERFACE's interrupt IN endpoint and hands each report to REPORT.
    int (*interrupt_open)(void *device, const struct usb_interface_info *interface,
                          usb_report_fn report, void *context);
    void (*interrupt_close)(void *device, const struct usb_interface_info *interface);
    void (*lock)(void);
    void (*unlock)(void);
};

struct usb_driver;

struct usb_dev {
    const struct usb_host_ops *ops;
    void *host_device;
    char name[24];
    uint16_t vendor;
    uint16_t product;
    uint16_t release;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    unsigned interface_count;
    struct usb_interface_info interfaces[USB_INTERFACES_MAX];
    struct usb_driver *bound[USB_INTERFACES_MAX];
    void *driver_data[USB_INTERFACES_MAX];
    struct usb_dev *next;
};

struct usb_device_id {
    uint16_t class_code;
    uint16_t subclass;
    uint16_t protocol;
};

struct usb_driver {
    const char *name;
    const struct usb_device_id *ids;
    unsigned id_count;
    int (*probe)(struct usb_dev *device, unsigned interface);
    void (*disconnect)(struct usb_dev *device, unsigned interface);
    struct usb_driver *next;
};

// From a host controller driver, with its lock held. DESCRIPTOR is the 18-byte device
// descriptor.
struct usb_dev *usb_device_added(const struct usb_host_ops *ops, void *host_device,
                                 const char *name, const uint8_t *descriptor,
                                 const struct usb_interface_info *interfaces, unsigned count);
void usb_device_removed(struct usb_dev *device);

int usb_register_driver(struct usb_driver *driver);
void usb_unregister_driver(struct usb_driver *driver);

#endif
