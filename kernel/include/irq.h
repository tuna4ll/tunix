#ifndef TUNIX_IRQ_H
#define TUNIX_IRQ_H

#include <stdint.h>

#define IRQ_VECTOR_FIRST 0x40U
#define IRQ_VECTOR_COUNT 16U

typedef void (*irq_handler_fn)(void *context);

unsigned irq_request(const char *name, const char *kind, irq_handler_fn handler,
                     void *context);

int irq_dispatch(unsigned vector);

void irq_release(unsigned vector);

int irq_describe(unsigned slot, unsigned *vector, uint64_t *count,
                 const char **name, const char **kind);

uint64_t irq_total(void);

#endif
