#ifndef TUNIX_SYSVSEM_H
#define TUNIX_SYSVSEM_H

#include <stdint.h>

int sysvsem_get(int32_t key, int count, int flags);
int64_t sysvsem_op(int id, uint64_t user_ops, unsigned count, int64_t timeout_ns);
int64_t sysvsem_ctl(int id, int num, int command, uint64_t argument);
void sysvsem_exit(int64_t tgid);

#endif
