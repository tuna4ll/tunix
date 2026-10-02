#ifndef TUNIX_KLOCK_H
#define TUNIX_KLOCK_H

#define KLOCK_HOLD_SLOTS 24U

struct klock_hold {
    uint32_t note;
    uint32_t count;
    uint64_t total_ns;
    uint64_t max_ns;
};

void klock_statistics_start(void);
void klock_statistics_stop(void);
int klock_statistics(unsigned index, struct klock_hold *out);
unsigned klock_shared_peak(void);

void kernel_lock(void);
void kernel_unlock(void);

void kernel_lock_shared(void);
void kernel_unlock_shared(void);

void kernel_unlock_current(void);
void kernel_exit(void);
void kernel_exit_from_isr(void);

int kernel_lock_release_for_wait(void);
void kernel_lock_retake_after_wait(int released);
void kernel_lock_wait_tick(void);
int kernel_lock_in_interrupt(void);

int kernel_lock_held_here(void);

void klock_note(uint32_t what);

#define KLOCK_NOTE_SYSCALL   0x10000U
#define KLOCK_NOTE_INTERRUPT 0x20000U
#define KLOCK_NOTE_FIRST_RUN 0x30000U
#define KLOCK_NOTE_IDLE      0x40000U

#define KLOCK_NOTE_IOCTL     0x50000U

void kernel_lock_from_isr(void);
void kernel_unlock_from_isr(void);

int kernel_lock_shared_here(void);

#endif
