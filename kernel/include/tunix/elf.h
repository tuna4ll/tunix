#ifndef TUNIX_ELF_H
#define TUNIX_ELF_H

struct process;
struct vfs_node;

int elf_load_process(struct process *process, struct vfs_node *file,
                     const char *const argv[], const char *const envp[]);

#endif
