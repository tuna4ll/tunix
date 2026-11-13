#ifndef TUNIX_KENTRY_H
#define TUNIX_KENTRY_H

void kernel_enter_from_isr(void);
void kernel_leave_from_isr(void);
void kernel_exit(void);
void kernel_exit_from_isr(void);

void kernel_overlap_sample(void);
void kernel_overlap_start(void);
void kernel_overlap_stop(void);
unsigned kernel_overlap_peak(void);

#endif
