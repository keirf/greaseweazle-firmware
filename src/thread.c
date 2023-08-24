/*
 * thread.c
 * 
 * Cooperative multitasking.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com> and Eric Anderson
 * <ejona86@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

/* Holds stack pointer. */
static uint32_t *waiting_thread;

__attribute__((naked))
static void _thread_yield(uint32_t *new_stack, uint32_t **save_stack_pointer)
{
    asm (
        "    stmdb sp!,{r4-r11,lr}\n"
        "    str   sp,[r1]\n"
        "    b     resume\n"
        );
}

void thread_yield(void)
{
    if (!waiting_thread)
        return;
    _thread_yield(waiting_thread, &waiting_thread);
}

__attribute__((naked))
static void resume(uint32_t *stack)
{
    asm (
        "    mov   sp,r0\n"
        "    ldmia sp!,{r4-r11,lr}\n"
        "    bx    lr\n"
        );
}

void _thread_main(struct thread *thread, void (*func)(void*), void *arg)
{
    uint32_t *other_thread;
    func(arg);
    thread->exited = TRUE;

    other_thread = waiting_thread;
    waiting_thread = 0;
    resume(other_thread);
    ASSERT(0); /* unreachable */
}

__attribute__((naked))
static void _main(void)
{
    asm (
        "    mov r0,r9\n"
        "    mov r1,r10\n"
        "    mov r2,r11\n"
        "    b   _thread_main\n"
        );
}

void thread_start(struct thread *thread, uint32_t *stack,
                  void (*func)(void *), void* arg)
{
    memset(thread, 0, sizeof(*thread));
    ASSERT(!waiting_thread);
    /* Fake thread_yield storage */
    *(--stack) = (uint32_t)_main; /* rl */
    *(--stack) = (uint32_t)arg; /* r11 */
    *(--stack) = (uint32_t)func; /* r10 */
    *(--stack) = (uint32_t)thread; /* r9 */
    stack -= 5; /* r4-r8 */
    waiting_thread = stack;
}

bool_t thread_tryjoin(struct thread *thread)
{
    bool_t exited = thread->exited;
    thread->exited = FALSE;
    return exited;
}

void thread_join(struct thread *thread)
{
    while (!thread->exited)
        thread_yield();
}

void thread_reset(void)
{
    waiting_thread = NULL;
}
