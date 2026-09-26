#include <stddef.h>
#include <stdint.h>
#include "include/process.h"
#include "include/usercopy.h"
#include "include/vmm.h"

int copy_from_user(void *destination, uint64_t user_source, size_t length) {
    struct process *process = process_current();
    if (!process) return -1;
    return vmm_copy_from_space(process->cr3, destination, user_source, length);
}

int copy_to_user(uint64_t user_destination, const void *source, size_t length) {
    struct process *process = process_current();
    if (!process) return -1;
    return vmm_copy_to_space(process->cr3, user_destination, source, length);
}

int copy_string_from_user(char *destination, size_t capacity, uint64_t user_source) {
    if (!destination || capacity == 0) return -1;
    size_t copied = 0;
    while (copied < capacity) {
        uint64_t address = user_source + copied;
        size_t chunk = 4096U - (size_t)(address & 4095U);
        if (chunk > capacity - copied) chunk = capacity - copied;
        if (copy_from_user(destination + copied, address, chunk) != 0) return -1;
        for (size_t index = 0; index < chunk; index++)
            if (!destination[copied + index]) return (int)(copied + index);
        copied += chunk;
    }
    destination[capacity - 1] = '\0';
    return -2;
}
