#ifndef TUNIX_SDHCI_H
#define TUNIX_SDHCI_H

#include <stdint.h>

int sdhci_attach(uint64_t registers, uint64_t clock_hz, int quirks);

#define SDHCI_QUIRK_WRITE_DELAY 1

#endif
