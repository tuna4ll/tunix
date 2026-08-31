#ifndef TUNIX_IRQ_H
#define TUNIX_IRQ_H

#include <stdint.h>

/*
 * Interrupts that belong to a driver rather than to the kernel.
 *
 * Everything this kernel answered before this file was wired by hand: the tick,
 * the two PS/2 lines and the SCI each have a stub of their own, a fixed vector,
 * and a case in isr_dispatch(). That works for devices the kernel knows it has
 * and stops working the moment a driver wants an interrupt of its own, which
 * every device that is not a keyboard does.
 *
 * A driver asks for a vector here, is given one out of a small pool, and points
 * it at whatever the device it owns needs. The pool sits above the legacy lines
 * the 8259s were remapped onto and below the vectors SMP reserved for itself,
 * so nothing here can collide with a line the firmware describes.
 *
 * Handlers run with interrupts off and the kernel lock held, exactly like a
 * syscall: isr_handler() takes the lock before dispatching. So a handler may
 * touch what a syscall may touch, and must not do anything that waits.
 */

#define IRQ_VECTOR_FIRST 0x40U
#define IRQ_VECTOR_COUNT 16U

typedef void (*irq_handler_fn)(void *context);

/* Claim a vector, or 0 when the pool is empty. `name` is not copied: it is
   reported as-is by /proc/interrupts, so it has to outlive the driver. */
unsigned irq_request(const char *name, irq_handler_fn handler, void *context);

/* Answer one. True when the vector belonged to a driver, which is also how a
   vector nobody claimed tells itself apart from a handler that did nothing. */
int irq_dispatch(unsigned vector);

/* Walk what has been claimed, for /proc/interrupts. Returns 0 while `slot` is
   in range, whether or not anything holds it. */
int irq_describe(unsigned slot, unsigned *vector, uint64_t *count,
                 const char **name);

/* How many interrupts have been delivered to drivers in total. The cheap
   version of the question "is any of this actually arriving". */
uint64_t irq_total(void);

#endif
