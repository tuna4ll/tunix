#if defined(TUNIX_KCOV)

#include <stddef.h>
#include <stdint.h>
#include <tunix/errno.h>
#include <tunix/file.h>
#include <tunix/heap.h>
#include <tunix/kcov.h>
#include <tunix/kstring.h>
#include <tunix/lock.h>
#include <tunix/percpu.h>
#include <tunix/pmm.h>
#include <tunix/process.h>
#include <tunix/vfs.h>
#include <tunix/vmm.h>

#define KCOV_INIT_TRACE32 0x80046301UL
#define KCOV_INIT_TRACE64 0x80086301UL
#define KCOV_ENABLE       0x6364UL
#define KCOV_DISABLE      0x6365UL
#define KCOV_RESET_TRACE  0x6368UL
#define KCOV_TRACE_PC     0U
#define KCOV_TRACE_CMP    1U
#define KCOV_MODE_OFF     0U
#define KCOV_MODE_PC      1U
#define KCOV_MODE_CMP     2U
#define KCOV_WORDS_MAX    (1ULL << 24)
#define KCOV_CMP_CONST    1ULL
#define KCOV_CMP_SIZE(n)  ((uint64_t)(n) << 1)
#define KCOV_RECORD_WORDS 4U

struct kcov {
    struct lock lock;
    uint32_t refs;
    uint64_t words;
    uint64_t pages;
    uint64_t physical;
    uint64_t *area;
    struct process *owner;
};

static uint32_t kcov_tasks;

static inline __attribute__((always_inline)) struct process *traced_task(uint32_t mode) {
    if (!__atomic_load_n(&kcov_tasks, __ATOMIC_RELAXED)) return NULL;
    struct cpu *self = cpu_current();
    if (!self || self->in_interrupt) return NULL;
    struct process *task = self->current;
    if (!task || __atomic_load_n(&task->kcov_mode, __ATOMIC_ACQUIRE) != mode) return NULL;
    return task;
}

void __sanitizer_cov_trace_pc(void) {
    struct process *task = traced_task(KCOV_MODE_PC);
    if (!task) return;
    uint64_t *area = task->kcov_area;
    uint64_t position = __atomic_load_n(&area[0], __ATOMIC_RELAXED) + 1U;
    if (position >= task->kcov_words) return;
    area[position] = (uint64_t)__builtin_return_address(0);
    __atomic_store_n(&area[0], position, __ATOMIC_RELAXED);
}

static inline __attribute__((always_inline)) void record_compare(uint64_t type, uint64_t first,
                                                                 uint64_t second, uint64_t pc) {
    struct process *task = traced_task(KCOV_MODE_CMP);
    if (!task) return;
    uint64_t *area = task->kcov_area;
    uint64_t count = __atomic_load_n(&area[0], __ATOMIC_RELAXED);
    uint64_t start = 1U + count * KCOV_RECORD_WORDS;
    if (start + KCOV_RECORD_WORDS > task->kcov_words) return;
    area[start] = type;
    area[start + 1U] = first;
    area[start + 2U] = second;
    area[start + 3U] = pc;
    __atomic_store_n(&area[0], count + 1U, __ATOMIC_RELAXED);
}

#define COMPARE_HOOKS(bits, log) \
    void __sanitizer_cov_trace_cmp##log(uint##bits##_t first, uint##bits##_t second) { \
        record_compare(KCOV_CMP_SIZE(log == 1       ? 0 \
                                         : log == 2 ? 1 \
                                         : log == 4 ? 2 \
                                                    : 3), \
                       first, second, (uint64_t)__builtin_return_address(0)); \
    } \
    void __sanitizer_cov_trace_const_cmp##log(uint##bits##_t first, uint##bits##_t second) { \
        record_compare(KCOV_CMP_SIZE(log == 1       ? 0 \
                                         : log == 2 ? 1 \
                                         : log == 4 ? 2 \
                                                    : 3) | \
                           KCOV_CMP_CONST, \
                       first, second, (uint64_t)__builtin_return_address(0)); \
    }

void __sanitizer_cov_trace_cmp1(uint8_t first, uint8_t second);
void __sanitizer_cov_trace_const_cmp1(uint8_t first, uint8_t second);
void __sanitizer_cov_trace_cmp2(uint16_t first, uint16_t second);
void __sanitizer_cov_trace_const_cmp2(uint16_t first, uint16_t second);
void __sanitizer_cov_trace_cmp4(uint32_t first, uint32_t second);
void __sanitizer_cov_trace_const_cmp4(uint32_t first, uint32_t second);
void __sanitizer_cov_trace_cmp8(uint64_t first, uint64_t second);
void __sanitizer_cov_trace_const_cmp8(uint64_t first, uint64_t second);
void __sanitizer_cov_trace_switch(uint64_t value, const uint64_t *cases);
void __sanitizer_cov_trace_pc(void);

COMPARE_HOOKS(8, 1)
COMPARE_HOOKS(16, 2)
COMPARE_HOOKS(32, 4)
COMPARE_HOOKS(64, 8)

void __sanitizer_cov_trace_switch(uint64_t value, const uint64_t *cases) {
    uint64_t count = cases[0];
    uint64_t type;
    switch (cases[1]) {
    case 8:  type = KCOV_CMP_SIZE(0); break;
    case 16: type = KCOV_CMP_SIZE(1); break;
    case 32: type = KCOV_CMP_SIZE(2); break;
    case 64: type = KCOV_CMP_SIZE(3); break;
    default: return;
    }
    uint64_t pc = (uint64_t)__builtin_return_address(0);
    for (uint64_t index = 0; index < count; index++)
        record_compare(type | KCOV_CMP_CONST, cases[2U + index], value, pc);
}

static void kcov_put(struct kcov *kcov) {
    if (__atomic_sub_fetch(&kcov->refs, 1U, __ATOMIC_ACQ_REL)) return;
    if (kcov->physical) pmm_free_pages((void *)kcov->physical, kcov->pages);
    kfree(kcov);
}

static void detach_locked(struct kcov *kcov, struct process *task) {
    __atomic_store_n(&task->kcov_mode, KCOV_MODE_OFF, __ATOMIC_RELEASE);
    task->kcov_area = NULL;
    task->kcov_words = 0;
    task->kcov = NULL;
    kcov->owner = NULL;
    __atomic_sub_fetch(&kcov_tasks, 1U, __ATOMIC_RELAXED);
}

void kcov_task_exit(struct process *process) {
    struct kcov *kcov = process ? process->kcov : NULL;
    if (!kcov) return;
    lock_acquire(&kcov->lock);
    if (kcov->owner == process) detach_locked(kcov, process);
    lock_release(&kcov->lock);
    kcov_put(kcov);
}

static int kcov_open(struct file *file) {
    struct kcov *kcov = kmalloc(sizeof(*kcov));
    if (!kcov) return -ENOMEM;
    memset(kcov, 0, sizeof(*kcov));
    lock_init(&kcov->lock, "kcov", LOCK_RANK_DEVICE);
    kcov->refs = 1;
    file->private_data = kcov;
    return 0;
}

static void kcov_release(struct file *file) {
    struct kcov *kcov = file->private_data;
    file->private_data = NULL;
    if (kcov) kcov_put(kcov);
}

static int64_t init_trace(struct kcov *kcov, uint64_t words) {
    if (words < 2U || words > KCOV_WORDS_MAX) return -EINVAL;
    uint64_t pages = (words * sizeof(uint64_t) + PMM_PAGE_SIZE - 1U) / PMM_PAGE_SIZE;
    void *physical = pmm_alloc_pages(pages, PMM_PAGE_SIZE);
    if (!physical) return -ENOMEM;
    uint64_t *area = (uint64_t *)vmm_phys_to_virt((uint64_t)physical);
    memset(area, 0, pages * PMM_PAGE_SIZE);
    lock_acquire(&kcov->lock);
    if (kcov->area) {
        lock_release(&kcov->lock);
        pmm_free_pages(physical, pages);
        return -EBUSY;
    }
    kcov->physical = (uint64_t)physical;
    kcov->pages = pages;
    kcov->words = words;
    kcov->area = area;
    lock_release(&kcov->lock);
    return 0;
}

static int64_t enable(struct kcov *kcov, uint64_t mode) {
    struct process *self = process_current();
    if (!self) return -EINVAL;
    if (mode != KCOV_TRACE_PC && mode != KCOV_TRACE_CMP) return -EINVAL;
    lock_acquire(&kcov->lock);
    int64_t status = 0;
    if (!kcov->area) status = -EINVAL;
    else if (kcov->owner || self->kcov) status = -EBUSY;
    if (status == 0) {
        __atomic_add_fetch(&kcov->refs, 1U, __ATOMIC_RELAXED);
        kcov->owner = self;
        self->kcov = kcov;
        self->kcov_area = kcov->area;
        self->kcov_words = kcov->words;
        __atomic_add_fetch(&kcov_tasks, 1U, __ATOMIC_RELAXED);
        __atomic_store_n(&self->kcov_mode, mode == KCOV_TRACE_PC ? KCOV_MODE_PC : KCOV_MODE_CMP,
                         __ATOMIC_RELEASE);
    }
    lock_release(&kcov->lock);
    return status;
}

static int64_t disable(struct kcov *kcov) {
    struct process *self = process_current();
    lock_acquire(&kcov->lock);
    if (!self || kcov->owner != self) {
        lock_release(&kcov->lock);
        return -EINVAL;
    }
    detach_locked(kcov, self);
    lock_release(&kcov->lock);
    kcov_put(kcov);
    return 0;
}

static int64_t kcov_ioctl(struct file *file, unsigned long request, uint64_t argument) {
    struct kcov *kcov = file->private_data;
    if (!kcov) return -EINVAL;
    switch (request) {
    case KCOV_INIT_TRACE32:
    case KCOV_INIT_TRACE64: return init_trace(kcov, argument);
    case KCOV_ENABLE:       return enable(kcov, argument);
    case KCOV_DISABLE:      return argument ? -EINVAL : disable(kcov);
    case KCOV_RESET_TRACE:  {
        lock_acquire(&kcov->lock);
        int64_t status = kcov->area ? 0 : -EINVAL;
        if (kcov->area) __atomic_store_n(&kcov->area[0], 0, __ATOMIC_RELAXED);
        lock_release(&kcov->lock);
        return status;
    }
    default: return -ENOTTY;
    }
}

static int64_t kcov_mmap(struct vfs_node *node, struct file *file, uint64_t cr3,
                         uint64_t virtual_address, uint64_t length, uint64_t offset,
                         uint64_t page_flags) {
    (void)node;
    struct kcov *kcov = file->private_data;
    if (!kcov || !length || offset) return -EINVAL;
    lock_acquire(&kcov->lock);
    uint64_t pages = (length + PMM_PAGE_SIZE - 1U) / PMM_PAGE_SIZE;
    if (!kcov->area || pages > kcov->pages) {
        lock_release(&kcov->lock);
        return -EINVAL;
    }
    uint64_t flags = page_flags | PAGE_USER | PAGE_PRESENT | PAGE_NX | PAGE_SHARED;
    uint64_t mapped = 0;
    int64_t result = 0;
    for (; mapped < pages; mapped++) {
        uint64_t physical = kcov->physical + mapped * PMM_PAGE_SIZE;
        if (pmm_page_ref(physical) != 0) {
            result = -ENOMEM;
            break;
        }
        if (vmm_map_page_in(cr3, virtual_address + mapped * PMM_PAGE_SIZE, physical, flags) != 0) {
            pmm_free_page((void *)physical);
            result = -ENOMEM;
            break;
        }
    }
    if (result != 0) {
        while (mapped) {
            mapped--;
            (void)vmm_unmap_page_in(cr3, virtual_address + mapped * PMM_PAGE_SIZE);
            pmm_free_page((void *)(kcov->physical + mapped * PMM_PAGE_SIZE));
        }
    }
    lock_release(&kcov->lock);
    return result;
}

static const struct vfs_file_ops kcov_file_ops = {
    .open = kcov_open,
    .release = kcov_release,
    .ioctl = kcov_ioctl,
};

void kcov_publish(void) {
    struct vfs_node *debug = vfs_mkdir_p("/sys/kernel/debug");
    if (!debug || vfs_find_entry(debug, "kcov")) return;
    struct vfs_node *node = vfs_alloc_node("kcov", VFS_FILE | VFS_UNSIZED);
    if (!node) return;
    node->mode = 0600;
    node->file_ops = &kcov_file_ops;
    node->mmap = kcov_mmap;
    if (vfs_attach(debug, node) != 0) return;
}

#endif
