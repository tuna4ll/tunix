#ifndef TUNIX_VMM_ARCH_H
#define TUNIX_VMM_ARCH_H

#include <stdint.h>

#include "vmm.h"

#if defined(__x86_64__)

#define PTE_ADDRESS_MASK 0x000FFFFFFFFFF000ULL

static inline int pte_present(uint64_t entry) {
    return (entry & PAGE_PRESENT) != 0;
}

static inline int pte_huge(uint64_t entry) {
    return (entry & PAGE_HUGE) != 0;
}

static inline uint64_t pte_address(uint64_t entry) {
    return entry & PTE_ADDRESS_MASK;
}

static inline uint64_t pte_flags(uint64_t entry) {
    return entry & ~PTE_ADDRESS_MASK;
}

static inline uint64_t pte_upper_flags(uint64_t entry) {
    return entry;
}

static inline uint64_t pte_leaf_flags(uint64_t entry) {
    return entry;
}

static inline uint64_t pte_page(uint64_t physical, uint64_t flags) {
    return physical | flags;
}

static inline uint64_t pte_block(uint64_t physical, uint64_t flags) {
    return physical | flags | PAGE_HUGE;
}

static inline uint64_t pte_table(uint64_t physical, uint64_t flags) {
    return physical | flags;
}

static inline uint64_t pte_retarget(uint64_t entry, uint64_t physical) {
    return physical | (entry & ~PTE_ADDRESS_MASK);
}

static inline void pte_table_grant_user(uint64_t *entry, uint64_t leaf_flags) {
    if ((leaf_flags & PAGE_USER) && !(*entry & PAGE_USER)) *entry |= PAGE_USER;
}

static inline uint64_t vmm_arch_read_root(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value & PTE_ADDRESS_MASK;
}

static inline void vmm_arch_write_root(uint64_t value) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

static inline void vmm_arch_invalidate(uint64_t address) {
    __asm__ volatile("invlpg (%0)" : : "r"(address) : "memory");
}

static inline int vmm_arch_direct_map_wanted(uint64_t physical) {
    (void)physical;
    return 1;
}

static inline void vmm_arch_sync_executable(uint64_t physical) {
    (void)physical;
}

#elif defined(__aarch64__)

#define PTE_ADDRESS_MASK 0x0000FFFFFFFFF000ULL

#define PTE_VALID (1ULL << 0)
#define PTE_NOT_BLOCK (1ULL << 1)
#define PTE_ATTR_NORMAL (0ULL << 2)
#define PTE_ATTR_DEVICE (1ULL << 2)
#define PTE_ATTR_NONCACHED (2ULL << 2)
#define PTE_ATTR_MASK (7ULL << 2)
#define PTE_AP_USER (1ULL << 6)
#define PTE_AP_READONLY (1ULL << 7)
#define PTE_SH_INNER (3ULL << 8)
#define PTE_AF (1ULL << 10)
#define PTE_NG (1ULL << 11)
#define PTE_PXN (1ULL << 53)
#define PTE_UXN (1ULL << 54)
#define PTE_SW_COW (1ULL << 55)
#define PTE_SW_SHARED (1ULL << 56)
#define PTE_SW_FILEBACKED (1ULL << 57)
#define PTE_SW_DEVICE (1ULL << 58)

static inline int pte_present(uint64_t entry) {
    return (entry & PTE_VALID) != 0;
}

static inline int pte_huge(uint64_t entry) {
    return (entry & (PTE_VALID | PTE_NOT_BLOCK)) == PTE_VALID;
}

static inline uint64_t pte_address(uint64_t entry) {
    return entry & PTE_ADDRESS_MASK;
}

static inline uint64_t pte_flags(uint64_t entry) {
    if (!(entry & PTE_VALID)) return 0;
    uint64_t flags = PAGE_PRESENT;
    if (!(entry & PTE_NOT_BLOCK)) flags |= PAGE_HUGE;
    if (!(entry & PTE_AP_READONLY)) flags |= PAGE_WRITE;
    if (entry & PTE_AP_USER) {
        flags |= PAGE_USER;
        if (entry & PTE_UXN) flags |= PAGE_NX;
    } else if (entry & PTE_PXN) {
        flags |= PAGE_NX;
    }
    if ((entry & PTE_ATTR_MASK) == PTE_ATTR_DEVICE) flags |= PAGE_UNCACHED;
    if (entry & PTE_SW_COW) flags |= PAGE_COW;
    if (entry & PTE_SW_SHARED) flags |= PAGE_SHARED;
    if (entry & PTE_SW_FILEBACKED) flags |= PAGE_FILEBACKED;
    if (entry & PTE_SW_DEVICE) flags |= PAGE_DEVICE;
    return flags;
}

static inline uint64_t pte_upper_flags(uint64_t entry) {
    if (pte_huge(entry)) return pte_flags(entry);
    if (!(entry & PTE_VALID)) return 0;
    return PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
}

static inline uint64_t pte_leaf_flags(uint64_t entry) {
    return pte_flags(entry);
}

static inline uint64_t pte_encode(uint64_t physical, uint64_t flags, int block) {
    uint64_t entry = (physical & PTE_ADDRESS_MASK) | PTE_VALID | PTE_AF | PTE_SH_INNER;
    if (!block) entry |= PTE_NOT_BLOCK;
    if (!block && (flags & PAGE_WRITE_COMBINING)) entry |= PTE_ATTR_NONCACHED;
    else if (flags & (PAGE_UNCACHED | PAGE_WRITE_THROUGH)) entry |= PTE_ATTR_DEVICE;
    if (!(flags & PAGE_WRITE)) entry |= PTE_AP_READONLY;
    if (flags & PAGE_USER) {
        entry |= PTE_AP_USER | PTE_NG | PTE_PXN;
        if (flags & PAGE_NX) entry |= PTE_UXN;
    } else {
        entry |= PTE_UXN;
        if (flags & PAGE_NX) entry |= PTE_PXN;
    }
    if (flags & PAGE_COW) entry |= PTE_SW_COW;
    if (flags & PAGE_SHARED) entry |= PTE_SW_SHARED;
    if (flags & PAGE_FILEBACKED) entry |= PTE_SW_FILEBACKED;
    if (flags & PAGE_DEVICE) entry |= PTE_SW_DEVICE;
    return entry;
}

static inline uint64_t pte_page(uint64_t physical, uint64_t flags) {
    return pte_encode(physical, flags, 0);
}

static inline uint64_t pte_block(uint64_t physical, uint64_t flags) {
    return pte_encode(physical, flags, 1);
}

static inline uint64_t pte_table(uint64_t physical, uint64_t flags) {
    (void)flags;
    return (physical & PTE_ADDRESS_MASK) | PTE_VALID | PTE_NOT_BLOCK;
}

static inline uint64_t pte_retarget(uint64_t entry, uint64_t physical) {
    return physical | (entry & ~PTE_ADDRESS_MASK);
}

static inline void pte_table_grant_user(uint64_t *entry, uint64_t leaf_flags) {
    (void)entry;
    (void)leaf_flags;
}

static inline uint64_t vmm_arch_read_root(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(value));
    return value & PTE_ADDRESS_MASK;
}

static inline void vmm_arch_write_root(uint64_t value) {
    __asm__ volatile("msr ttbr0_el1, %0; msr ttbr1_el1, %0; isb; tlbi vmalle1; dsb sy; isb"
                     : : "r"(value) : "memory");
}

static inline void vmm_arch_invalidate(uint64_t address) {
    __asm__ volatile("tlbi vaae1, %0; dsb sy; isb" : : "r"(address >> 12) : "memory");
}

int aarch64_physical_is_ram(uint64_t physical);

static inline int vmm_arch_direct_map_wanted(uint64_t physical) {
    return aarch64_physical_is_ram(physical);
}

static inline void vmm_arch_sync_executable(uint64_t physical) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t start = DIRECT_MAP_BASE + (physical & ~0xFFFULL);
    if (!(ctr & (1ULL << 28))) {
        uint64_t line = 4ULL << ((ctr >> 16) & 0xFU);
        for (uint64_t address = start; address < start + 4096ULL; address += line)
            __asm__ volatile("dc cvau, %0" : : "r"(address) : "memory");
    }
    if (!(ctr & (1ULL << 29))) __asm__ volatile("ic ialluis" ::: "memory");
}

#else
#error "no page table encoding for this architecture"
#endif

#endif
