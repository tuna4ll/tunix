#include <stddef.h>
#include <stdint.h>

#include "../../include/dma.h"
#include "../../include/kstring.h"
#include "../../include/virtio.h"
#include "../../include/net/virtio_net.h"

#define VIRTIO_NET_DEVICE_ID 0x1041U
#define VIRTIO_NET_F_MAC 5U

#define VIRTIO_NET_RX_QUEUE 0U
#define VIRTIO_NET_TX_QUEUE 1U
#define VIRTIO_NET_RX_SLOTS 128U
#define VIRTIO_NET_BUFFER_BYTES 1536U
#define ETHERNET_FRAME_MIN 14U
#define ETHERNET_FRAME_MAX 1514U

struct virtio_net_header {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t header_length;
    uint16_t gso_size;
    uint16_t checksum_start;
    uint16_t checksum_offset;
    /* Part of the version-1 header even without merged receive buffers. */
    uint16_t buffer_count;
} __attribute__((packed));

_Static_assert(sizeof(struct virtio_net_header) == 12U, "virtio-net header size");

struct receive_slot {
    uint8_t bytes[VIRTIO_NET_BUFFER_BYTES];
} __attribute__((aligned(16)));

static struct virtio_device device;
static struct virtio_queue receive_queue;
static struct virtio_queue transmit_queue;
static struct receive_slot *receive_slots;
static uint64_t receive_physical;
static uint8_t *transmit_buffer;
static uint64_t transmit_physical;
static uint8_t mac_address[6];
static unsigned receive_slot_count;
static int available;
static uint64_t received_packets;
static uint64_t transmitted_packets;
static uint64_t dropped_packets;

static int post_receive(unsigned index) {
    struct virtio_buffer buffer = {
        .physical = receive_physical + (uint64_t)index * sizeof(struct receive_slot),
        .length = sizeof(struct receive_slot)
    };
    memset(&receive_slots[index], 0, sizeof(receive_slots[index]));
    return virtio_queue_post(&receive_queue, &buffer, 1, 0);
}

static void network_interrupt(void *context) {
    (void)context;
    /* Reading the ISR acknowledges every reason represented by this byte. The
       stack is entered only from poll, never halfway through another call. */
    if (device.isr) (void)*device.isr;
}

int virtio_net_init(void) {
    available = 0;
    uint64_t features = 0;
    if (virtio_pci_attach(&device, VIRTIO_NET_DEVICE_ID,
                          1ULL << VIRTIO_NET_F_MAC, &features) != 0) return -1;
    if (!(features & (1ULL << VIRTIO_NET_F_MAC)) || !device.config) {
        virtio_pci_set_failed(&device);
        return -1;
    }

    for (unsigned index = 0; index < sizeof(mac_address); index++)
        mac_address[index] = device.config[index];

    (void)virtio_pci_request_irq(&device, "virtio-net", network_interrupt, NULL);
    if (virtio_pci_setup_queue(&device, &receive_queue, VIRTIO_NET_RX_QUEUE) != 0 ||
        virtio_pci_setup_queue(&device, &transmit_queue, VIRTIO_NET_TX_QUEUE) != 0) {
        virtio_pci_set_failed(&device);
        return -1;
    }
    /* Receive completions wake the device vector; transmit is synchronously
       reclaimed by its caller, so an interrupt would announce known work. */
    transmit_queue.interrupt_driven = 0;

    receive_slot_count = receive_queue.size;
    if (receive_slot_count > VIRTIO_NET_RX_SLOTS)
        receive_slot_count = VIRTIO_NET_RX_SLOTS;
    receive_slots = (struct receive_slot *)dma_alloc(
        (uint64_t)receive_slot_count * sizeof(*receive_slots), 16, &receive_physical);
    transmit_buffer = (uint8_t *)dma_alloc(
        sizeof(struct virtio_net_header) + ETHERNET_FRAME_MAX, 16, &transmit_physical);
    if (!receive_slots || !transmit_buffer) {
        virtio_pci_set_failed(&device);
        return -1;
    }

    /* A queue kick made before DRIVER_OK may be ignored. Publish readiness
       first, then make every receive buffer visible and notify the device. */
    virtio_pci_set_driver_ok(&device);
    for (unsigned index = 0; index < receive_slot_count; index++) {
        if (post_receive(index) != 0) {
            virtio_pci_set_failed(&device);
            return -1;
        }
    }

    received_packets = transmitted_packets = dropped_packets = 0;
    available = 1;
    return 0;
}

int virtio_net_present(void) { return available; }
const uint8_t *virtio_net_mac(void) { return mac_address; }
unsigned virtio_net_interrupt_vector(void) { return device.vector; }
uint64_t virtio_net_rx_packets(void) { return received_packets; }
uint64_t virtio_net_tx_packets(void) { return transmitted_packets; }
uint64_t virtio_net_rx_dropped(void) { return dropped_packets; }

int virtio_net_transmit(const void *frame, size_t length) {
    if (!available || !frame || length < ETHERNET_FRAME_MIN ||
        length > ETHERNET_FRAME_MAX) return -1;
    memset(transmit_buffer, 0, sizeof(struct virtio_net_header));
    memcpy(transmit_buffer + sizeof(struct virtio_net_header), frame, length);
    struct virtio_buffer buffer = {
        .physical = transmit_physical,
        .length = (uint32_t)(sizeof(struct virtio_net_header) + length)
    };
    if (virtio_queue_submit(&transmit_queue, &buffer, 1, 1) != 0) return -1;
    transmitted_packets++;
    return 0;
}

void virtio_net_poll(virtio_net_receive_fn receive) {
    if (!available || !receive) return;
    for (unsigned served = 0; served < 64U; served++) {
        uint64_t address;
        uint32_t length;
        if (!virtio_queue_take_used(&receive_queue, &address, &length)) break;
        if (address < receive_physical ||
            address >= receive_physical +
                       (uint64_t)receive_slot_count * sizeof(*receive_slots) ||
            (address - receive_physical) % sizeof(*receive_slots)) {
            dropped_packets++;
            continue;
        }
        unsigned index = (unsigned)((address - receive_physical) / sizeof(*receive_slots));
        if (length > sizeof(struct virtio_net_header) &&
            length <= sizeof(struct receive_slot)) {
            receive(receive_slots[index].bytes + sizeof(struct virtio_net_header),
                    length - sizeof(struct virtio_net_header));
            received_packets++;
        } else {
            dropped_packets++;
        }
        if (post_receive(index) != 0) dropped_packets++;
    }
}
