#include "include/heap.h"
#include "include/vmm.h"
#include "include/pmm.h"
#include "include/spinlock.h"

#define HEAP_START 0xFFFFFFFFC0000000
#define HEAP_INITIAL_SIZE (1024 * 1024)
#define HEAP_MAGIC 0x1234ABCD

typedef struct heap_block {
    uint32_t magic;
    uint32_t size;
    uint8_t is_free;
    struct heap_block* next;
} heap_block_t;

static heap_block_t* head = NULL;
static spinlock_t heap_lock;

extern void kprintf(const char *fmt, ...);
extern void panic(const char *msg);

void heap_init(void) {
    spinlock_init(&heap_lock);
    
    for (uint64_t i = 0; i < HEAP_INITIAL_SIZE; i += 4096) {
        void* phys = pmm_alloc_page();
        if (!phys) panic("HEAP: PMM out of memory!");
        vmm_map_page(HEAP_START + i, (uint64_t)phys, PAGE_PRESENT | PAGE_WRITE);
    }
    
    head = (heap_block_t*)HEAP_START;
    head->magic = HEAP_MAGIC;
    head->size = HEAP_INITIAL_SIZE - sizeof(heap_block_t);
    head->is_free = 1;
    head->next = NULL;
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    
    spinlock_acquire(&heap_lock);
    
    heap_block_t* curr = head;
    while (curr != NULL) {
        if (curr->is_free && curr->size >= size) {
            if (curr->size > size + sizeof(heap_block_t) + 16) {
                heap_block_t* new_block = (heap_block_t*)((uint8_t*)curr + sizeof(heap_block_t) + size);
                new_block->magic = HEAP_MAGIC;
                new_block->size = curr->size - size - sizeof(heap_block_t);
                new_block->is_free = 1;
                new_block->next = curr->next;
                
                curr->size = size;
                curr->next = new_block;
            }
            
            curr->is_free = 0;
            spinlock_release(&heap_lock);
            return (void*)((uint8_t*)curr + sizeof(heap_block_t));
        }
        curr = curr->next;
    }
    
    spinlock_release(&heap_lock);
    panic("HEAP: Out of memory!");
    return NULL;
}

void kfree(void* ptr) {
    if (!ptr) return;
    
    spinlock_acquire(&heap_lock);
    
    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
    if (block->magic != HEAP_MAGIC) {
        spinlock_release(&heap_lock);
        panic("HEAP: Invalid kfree magic!");
    }
    
    block->is_free = 1;
    
    heap_block_t* curr = head;
    while (curr != NULL) {
        if (curr->is_free && curr->next != NULL && curr->next->is_free) {
            curr->size += curr->next->size + sizeof(heap_block_t);
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
    
    spinlock_release(&heap_lock);
}
