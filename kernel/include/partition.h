#ifndef TUNIX_PARTITION_H
#define TUNIX_PARTITION_H

/* Read the partition table of every registered disk and register what it
   describes as devices of their own. See partition.c. */
void partition_scan(void);

#endif
