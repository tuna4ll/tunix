#ifndef TUNIX_XHCI_H
#define TUNIX_XHCI_H

#include <stdint.h>

/*
 * What the bring-up found, for the layers above it. Every one of these
 * addresses is read out of the controller rather than assumed: xHCI puts its
 * four register blocks wherever it likes and states the offsets in the first.
 */
struct xhci_controller {
    int present;
    uint16_t version;
    uint64_t base;         /* capability registers */
    uint64_t operational;  /* base + CAPLENGTH */
    uint64_t runtime;      /* base + RTSOFF: interrupters live here */
    uint64_t doorbell;     /* base + DBOFF: one dword per slot */
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t context_bytes; /* 32 or 64, and the difference is not cosmetic */
};

int xhci_init(void);
struct xhci_controller *xhci_get(void);

/* Collect whatever the HID endpoints have reported and ask for more. Called
   from the input layer's poll, so USB keys arrive by the same route PS/2 ones
   do rather than needing an interrupt path of their own. */
void xhci_poll(void);

/* Whether a HID device of each kind was enumerated. The input layer asks
   because the device nodes are created once, at boot, and a machine whose
   pointer is on USB has no PS/2 mouse to answer for it. */
int xhci_keyboard_present(void);
int xhci_pointer_present(void);

/* Register access. Wide reads are split in two on purpose; see the source. */
uint32_t xhci_read32(uint64_t address);
void xhci_write32(uint64_t address, uint32_t value);
uint64_t xhci_read64(uint64_t address);
void xhci_write64(uint64_t address, uint64_t value);

#endif
