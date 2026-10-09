#ifndef TUNIX_DEVFS_H
#define TUNIX_DEVFS_H

void devfs_init(void);
void devfs_add_block(int index);
void devfs_publish_sound(void);
void devfs_remove_sound(void);

#endif
