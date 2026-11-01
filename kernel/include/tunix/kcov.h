#ifndef TUNIX_KCOV_H
#define TUNIX_KCOV_H

struct process;

#if defined(TUNIX_KCOV)
void kcov_publish(void);
void kcov_task_exit(struct process *process);
#else
static inline void kcov_publish(void) {}
static inline void kcov_task_exit(struct process *process) { (void)process; }
#endif

#endif
