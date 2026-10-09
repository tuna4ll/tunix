#include <stddef.h>
#include <stdint.h>

#include <tunix/defer.h>
#include <tunix/lock.h>
#include <tunix/process.h>
#include <tunix/workqueue.h>

extern void kprintf(const char *fmt, ...);

static struct lock queue_lock = LOCK_INITIALIZER("work queue", LOCK_RANK_LEAF);
static struct work *head;
static struct work *tail;
static const char worker_channel;

void work_queue(struct work *work) {
    if (!work || __atomic_exchange_n(&work->queued, 1, __ATOMIC_ACQ_REL)) return;
    lock_acquire(&queue_lock);
    work->next = NULL;
    if (tail) tail->next = work;
    else head = work;
    tail = work;
    lock_release(&queue_lock);
    process_wake_all(&worker_channel);
}

static struct work *take_all(void) {
    lock_acquire(&queue_lock);
    struct work *list = head;
    head = tail = NULL;
    lock_release(&queue_lock);
    return list;
}

static void worker(void *unused) {
    (void)unused;
    for (;;) {
        process_prepare_wait(&worker_channel, 0);
        struct work *list = take_all();
        if (!list) {
            process_wait();
            process_finish_wait();
            continue;
        }
        process_finish_wait();
        defer_kernel_enter();
        while (list) {
            struct work *work = list;
            list = work->next;
            __atomic_store_n(&work->queued, 0, __ATOMIC_RELEASE);
            work->run(work->argument);
        }
        defer_kernel_leave();
        defer_poll();
    }
}

void workqueue_init(void) {
    if (!process_create_kthread("kworker", worker, NULL))
        kprintf("WORK: cannot start the worker thread\n");
}
