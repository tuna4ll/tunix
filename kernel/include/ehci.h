#ifndef TUNIX_EHCI_H
#define TUNIX_EHCI_H

#include <stdint.h>

/*
 * What the bring-up found. The register block is one contiguous mapping with
 * the operational half at an offset the capability half states, so the second
 * address is read out of the controller rather than assumed.
 */
struct ehci_controller {
    int present;
    uint16_t version;
    uint64_t base;         /* capability registers */
    uint64_t operational;  /* base + CAPLENGTH */
    unsigned ports;
};

int ehci_init(void);
struct ehci_controller *ehci_get(void);

#endif
