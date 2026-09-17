#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <kos/atomic.h>
#include <kos/compiler.h>
#include <kos/cpu.h>
#include <kos/heap.h>
#include <kos/list.h>
#include <kos/log.h>
#include <kos/memory.h>
#include <kos/process.h>
#include <kos/handle.h>
#include <kos/wait.h>
#include <kos/spinlock.h>
#include <kos/task.h>

extern struct task idle_task;

static struct process processes[PROCESS_MAX_COUNT];
static struct list_head process_list = LIST_HEAD_INIT(process_list);
static kos_spinlock_t process_lock = SPINLOCK_INIT;
static uint64_t next_pid = 2;
static volatile uint64_t active_process_count = 0;

static struct process idle_process;
static struct process kernel_process;

static kos_spinlock_t reaper_lock = SPINLOCK_INIT;
static struct list_head reaper_queue = LIST_HEAD_INIT(reaper_queue);
static struct thread *reaper_thread_ptr = NULL;
static volatile uint64_t reaper_reaped_total = 0;
static bool process_initialized = false;

struct process *process_get_kernel(void) {
    return &kernel_process;
}

struct process *process_get_idle(void) {
    return &idle_process;
}

struct process *process_get_current(void) {
    struct thread *cur = thread_current();
    if (cur != NULL && cur->process != NULL) {
        return cur->process;
    }
    return &kernel_process;
}

struct thread *thread_current(void) {
    return task_current();
}

enum thread_state thread_get_state(const struct thread *thread) {
    return thread ? thread->state : THREAD_STATE_UNUSED;
}

const char *thread_state_name(enum thread_state state) {
    switch (state) {
        case THREAD_STATE_UNUSED:   return "UNUSED";
        case THREAD_STATE_RUNNABLE: return "RUNNABLE";
        case THREAD_STATE_RUNNING:  return "RUNNING";
        case THREAD_STATE_BLOCKED:  return "BLOCKED";
        case THREAD_STATE_SLEEPING: return "SLEEPING";
        case THREAD_STATE_ZOMBIE:   return "ZOMBIE";
        case THREAD_STATE_DEAD:     return "DEAD";
        default:                    return "UNKNOWN";
    }
}

const char *process_state_name(enum process_state state) {
    switch (state) {
        case PROCESS_STATE_UNUSED: return "UNUSED";
        case PROCESS_STATE_ACTIVE: return "ACTIVE";
        case PROCESS_STATE_ZOMBIE: return "ZOMBIE";
        case PROCESS_STATE_DEAD:   return "DEAD";
        default:                   return "UNKNOWN";
    }
}

struct thread *reaper_get_thread(void) {
    return reaper_thread_ptr;
}

uint64_t reaper_reaped_count(void) {
    return __atomic_load_n(&reaper_reaped_total, __ATOMIC_RELAXED);
}

uint64_t process_count(void) {
    return __atomic_load_n(&active_process_count, __ATOMIC_RELAXED);
}

struct process *process_find_by_pid(uint64_t pid) {
    if (pid == 0) {
        return &idle_process;
    }
    if (pid == 1) {
        return &kernel_process;
    }
    uint64_t flags = spinlock_lock_irqsave(&process_lock);
    struct list_head *pos = process_list.next;
    while (pos != &process_list) {
        struct process *proc = list_entry(pos, struct process, process_node);
        if (proc->pid == pid) {
            spinlock_unlock_irqrestore(&process_lock, flags);
            return proc;
        }
        pos = pos->next;
    }
    spinlock_unlock_irqrestore(&process_lock, flags);
    return NULL;
}

struct process *process_create(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    uint64_t flags = spinlock_lock_irqsave(&process_lock);
    struct process *proc = NULL;
    for (size_t i = 0; i < PROCESS_MAX_COUNT; i++) {
        if (processes[i].state == PROCESS_STATE_UNUSED || processes[i].state == PROCESS_STATE_DEAD) {
            proc = &processes[i];
            break;
        }
    }
    if (proc == NULL) {
        spinlock_unlock_irqrestore(&process_lock, flags);
        return NULL;
    }

    proc->pid = next_pid++;
    struct process *cur = process_get_current();
    proc->ppid = cur ? cur->pid : 1;
    proc->state = PROCESS_STATE_ACTIVE;
    proc->exit_code = 0;
    list_init(&proc->threads);
    proc->thread_count = 0;
    proc->pml4_physical = cpu_read_cr3() & ~0xFFFull;
    spinlock_init(&proc->lock);
    list_init(&proc->process_node);
    proc->handles = handle_table_create();
    wait_queue_init(&proc->exit_wq);

    size_t len = 0;
    while (name[len] != '\0' && len < sizeof(proc->name) - 1) {
        proc->name[len] = name[len];
        len++;
    }
    proc->name[len] = '\0';

    list_add_tail(&proc->process_node, &process_list);
    __atomic_fetch_add(&active_process_count, 1, __ATOMIC_RELAXED);

    spinlock_unlock_irqrestore(&process_lock, flags);
    return proc;
}

void process_destroy(struct process *proc) {
    if (proc == NULL || proc->pid <= 1) {
        return;
    }
    struct handle_table *ht = NULL;
    uint64_t flags = spinlock_lock_irqsave(&process_lock);
    if (proc->state != PROCESS_STATE_DEAD) {
        proc->state = PROCESS_STATE_DEAD;
        list_del_init(&proc->process_node);
        if (active_process_count > 2) {
            __atomic_fetch_sub(&active_process_count, 1, __ATOMIC_RELAXED);
        }
    }
    ht = proc->handles;
    proc->handles = NULL;
    spinlock_unlock_irqrestore(&process_lock, flags);

    if (ht != NULL) {
        handle_table_destroy(ht);
    }

    wait_queue_wake_all(&proc->exit_wq);
}

void process_exit(struct process *proc, int exit_code) {
    if (proc == NULL || proc->pid <= 1) {
        return;
    }
    uint64_t flags = spinlock_lock_irqsave(&proc->lock);
    if (proc->state == PROCESS_STATE_ZOMBIE || proc->state == PROCESS_STATE_DEAD) {
        spinlock_unlock_irqrestore(&proc->lock, flags);
        return;
    }
    proc->exit_code = exit_code;
    proc->state = PROCESS_STATE_ZOMBIE;

    /* 1. Close handle table to trigger object unref and destructors */
    struct handle_table *ht = proc->handles;
    proc->handles = NULL;

    /* 2. Transition remaining non-current threads to ZOMBIE and enqueue to reaper */
    struct thread *cur = thread_current();
    struct list_head *pos = proc->threads.next;
    while (pos != &proc->threads) {
        struct thread *t = list_entry(pos, struct thread, process_node);
        pos = pos->next;
        if (t != cur && t->state != THREAD_STATE_ZOMBIE && t->state != THREAD_STATE_DEAD) {
            t->exit_code = exit_code;
            t->state = THREAD_STATE_ZOMBIE;
            task_sleep_dequeue(t);
            uint64_t rflags = spinlock_lock_irqsave(&reaper_lock);
            list_add_tail(&t->reaper_node, &reaper_queue);
            if (reaper_thread_ptr != NULL) {
                task_unblock(reaper_thread_ptr);
            }
            spinlock_unlock_irqrestore(&reaper_lock, rflags);
        }
    }

    bool has_no_threads = (proc->thread_count == 0);
    spinlock_unlock_irqrestore(&proc->lock, flags);

    if (ht != NULL) {
        handle_table_destroy(ht);
    }

    /* 3. Broadcast wakeup to all threads/processes waiting on exit_wq */
    wait_queue_wake_all(&proc->exit_wq);

    if (has_no_threads) {
        process_destroy(proc);
    }

    if (cur != NULL && cur->process == proc) {
        thread_exit(exit_code);
    }
}

static bool process_exit_condition(void *arg) {
    struct process *proc = (struct process *)arg;
    if (proc == NULL) {
        return true;
    }
    return (proc->state == PROCESS_STATE_ZOMBIE || proc->state == PROCESS_STATE_DEAD);
}

bool process_wait(struct process *proc, int *exit_code, uint64_t timeout_ms) {
    if (proc == NULL) {
        return false;
    }

    bool exited = false;
    if (timeout_ms == 0) {
        exited = process_exit_condition(proc);
    } else if (timeout_ms == UINT64_MAX) {
        wait_event(&proc->exit_wq, process_exit_condition, proc);
        exited = process_exit_condition(proc);
    } else {
        exited = wait_event_timeout(&proc->exit_wq, process_exit_condition, proc, timeout_ms);
    }

    if (exited) {
        if (exit_code != NULL) {
            *exit_code = proc->exit_code;
        }
        return true;
    }

    return false;
}

struct thread *thread_create(struct process *proc, void (*entry)(void *), void *arg, const char *name) {
    if (entry == NULL) {
        return NULL;
    }
    if (proc == NULL) {
        proc = &kernel_process;
    }
    if (name == NULL) {
        name = "thread";
    }

    struct thread *t = task_allocate_slot();
    if (t == NULL) {
        return NULL;
    }

    uint8_t *stack = kmalloc(TASK_KERNEL_STACK_SIZE);
    if (stack == NULL) {
        task_free_slot(t);
        return NULL;
    }

    uint64_t stack_top = ((uint64_t)stack + TASK_KERNEL_STACK_SIZE) & ~0xfull;
    stack_top -= sizeof(uint64_t);
    *(uint64_t *)stack_top = 0;

    uint64_t tid = task_next_id();

    *t = (struct thread){
        .context = {
            .rsp = stack_top,
            .rip = (uint64_t)task_start_trampoline,
            .rflags = 0x202,
        },
        .entry = entry,
        .argument = arg,
        .stack = stack,
        .stack_size = TASK_KERNEL_STACK_SIZE,
        .name = name,
        .state = THREAD_STATE_RUNNABLE,
        .tid = tid,
        .id = tid,
        .process = proc,
        .exit_code = 0,
        .sleep_target_tick = 0,
        .preempt_count = 0,
        .reschedule_pending = false,
        .quantum_remaining = TASK_DEFAULT_QUANTUM,
    };
    list_init(&t->sleep_node);
    list_init(&t->reaper_node);
    list_init(&t->process_node);

    uint64_t pflags = spinlock_lock_irqsave(&proc->lock);
    list_add_tail(&t->process_node, &proc->threads);
    proc->thread_count++;
    spinlock_unlock_irqrestore(&proc->lock, pflags);

    return t;
}

/* Dedicated reaper thread loop running on its own stack */
static void reaper_thread_entry(void *arg) {
    (void)arg;
    for (;;) {
        struct thread *zombie = NULL;
        uint64_t flags = spinlock_lock_irqsave(&reaper_lock);
        if (!list_empty(&reaper_queue)) {
            struct list_head *first = reaper_queue.next;
            list_del_init(first);
            zombie = list_entry(first, struct thread, reaper_node);
        } else {
            /* Queue empty: mark blocked while holding spinlock */
            if (reaper_thread_ptr != NULL) {
                reaper_thread_ptr->state = THREAD_STATE_BLOCKED;
            }
        }
        spinlock_unlock_irqrestore(&reaper_lock, flags);

        if (zombie != NULL) {
            /* 1. Transition state from ZOMBIE to DEAD */
            zombie->state = THREAD_STATE_DEAD;

            /* 2. Safely free kernel stack outside of zombie's running context */
            if (zombie->stack != NULL && zombie->stack_size > 0) {
                (void)kfree(zombie->stack);
                zombie->stack = NULL;
            }

            /* 3. Decrement owning process thread count */
            struct process *proc = zombie->process;
            if (proc != NULL) {
                uint64_t pflags = spinlock_lock_irqsave(&proc->lock);
                list_del_init(&zombie->process_node);
                if (proc->thread_count > 0) {
                    proc->thread_count--;
                }
                bool should_destroy = (proc->state == PROCESS_STATE_ZOMBIE && proc->thread_count == 0);
                spinlock_unlock_irqrestore(&proc->lock, pflags);

                if (should_destroy) {
                    process_destroy(proc);
                }
            }

            __atomic_fetch_add(&reaper_reaped_total, 1, __ATOMIC_RELAXED);
        } else {
            /* No zombies: switch away until unblocked by thread_exit */
            schedule();
        }
    }
}

KOS_NORETURN void thread_exit(int exit_code) {
    struct thread *cur = thread_current();
    if (cur == NULL || cur == &idle_task || cur->tid == 0 || cur->tid == 1) {
        /* Idle or bootstrap kernel thread must never exit */
        for (;;) {
            cpu_halt();
        }
    }

    cur->exit_code = exit_code;
    cur->state = THREAD_STATE_ZOMBIE;

    /* Enqueue onto reaper_queue and wake up reaper thread */
    uint64_t flags = spinlock_lock_irqsave(&reaper_lock);
    list_add_tail(&cur->reaper_node, &reaper_queue);
    if (reaper_thread_ptr != NULL) {
        task_unblock(reaper_thread_ptr);
    }
    spinlock_unlock_irqrestore(&reaper_lock, flags);

    /* Switch away: exiting thread never runs again */
    schedule();

    for (;;) {
        cpu_halt();
    }
}

bool process_subsystem_init(void) {
    if (process_initialized) {
        return true;
    }

    spinlock_init(&process_lock);
    list_init(&process_list);
    spinlock_init(&reaper_lock);
    list_init(&reaper_queue);
    reaper_reaped_total = 0;

    for (size_t i = 0; i < PROCESS_MAX_COUNT; i++) {
        processes[i].state = PROCESS_STATE_UNUSED;
        processes[i].pid = 0;
        processes[i].thread_count = 0;
        list_init(&processes[i].threads);
        list_init(&processes[i].process_node);
        spinlock_init(&processes[i].lock);
        processes[i].handles = NULL;
        wait_queue_init(&processes[i].exit_wq);
    }

    /* 1. Establish Idle Process (PID 0) */
    idle_process.pid = 0;
    idle_process.ppid = 0;
    idle_process.state = PROCESS_STATE_ACTIVE;
    idle_process.exit_code = 0;
    list_init(&idle_process.threads);
    idle_process.thread_count = 1;
    idle_process.pml4_physical = cpu_read_cr3() & ~0xFFFull;
    spinlock_init(&idle_process.lock);
    list_init(&idle_process.process_node);
    {
        const char *name = "idle";
        size_t i = 0;
        for (; name[i]; i++) idle_process.name[i] = name[i];
        idle_process.name[i] = '\0';
    }
    list_add_tail(&idle_process.process_node, &process_list);
    idle_process.handles = handle_table_create();
    wait_queue_init(&idle_process.exit_wq);

    /* Associate idle task with idle process */
    struct thread *itask = &idle_task;
    itask->process = &idle_process;
    list_init(&itask->process_node);
    list_add_tail(&itask->process_node, &idle_process.threads);

    /* 2. Establish Kernel Bootstrap Process (PID 1) */
    kernel_process.pid = 1;
    kernel_process.ppid = 0;
    kernel_process.state = PROCESS_STATE_ACTIVE;
    kernel_process.exit_code = 0;
    list_init(&kernel_process.threads);
    kernel_process.thread_count = 1;
    kernel_process.pml4_physical = cpu_read_cr3() & ~0xFFFull;
    spinlock_init(&kernel_process.lock);
    list_init(&kernel_process.process_node);
    {
        const char *name = "kernel";
        size_t i = 0;
        for (; name[i]; i++) kernel_process.name[i] = name[i];
        kernel_process.name[i] = '\0';
    }
    list_add_tail(&kernel_process.process_node, &process_list);
    kernel_process.handles = handle_table_create();
    wait_queue_init(&kernel_process.exit_wq);

    /* Associate bootstrap task with kernel process */
    struct thread *cur = task_current();
    if (cur != NULL) {
        cur->process = &kernel_process;
        list_init(&cur->process_node);
        list_add_tail(&cur->process_node, &kernel_process.threads);
    }

    active_process_count = 2;

    /* 3. Spawn dedicated reaper thread */
    reaper_thread_ptr = thread_create(&kernel_process, reaper_thread_entry, NULL, "reaper");
    if (reaper_thread_ptr != NULL) {
        reaper_thread_ptr->state = THREAD_STATE_BLOCKED;
    }

    process_initialized = true;
    return true;
}
