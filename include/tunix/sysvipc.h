#ifndef TUNIX_SYSVIPC_H
#define TUNIX_SYSVIPC_H

// Removes the SysV objects of an IPC namespace that is going away.
void sysvshm_namespace_gone(const void *ns);
void sysvsem_namespace_gone(const void *ns);

#endif
