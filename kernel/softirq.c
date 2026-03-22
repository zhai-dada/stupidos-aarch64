#include "softirq.h"

#include "lib/libasm.h"
#include "lib/libirq.h"
#include "lib/libmem.h"
#include "sched.h"
#include "smp.h"

typedef void (*softirq_action_t)(uint32_t cpu_id);

struct softirq_cpu_state
{
    uint32_t pending;
    bool in_softirq;
    struct tasklet_struct *tasklet_head;
    struct tasklet_struct *tasklet_tail;
};

static struct softirq_cpu_state softirq_state[CONFIG_MAX_CPUS];
static softirq_action_t softirq_vec[NR_SOFTIRQS];

static void tasklet_softirq_action(uint32_t cpu_id)
{
    struct tasklet_struct *tasklet;
    struct tasklet_struct *next;
    struct softirq_cpu_state *state;

    state = &softirq_state[cpu_id];

    /*
     * 先整体摘链，再逐个执行。
     * 这样 tasklet 回调里如果再次 schedule 自己，会进入下一轮 softirq，
     * 不会把当前链表遍历状态弄乱。
     */
    tasklet = state->tasklet_head;
    state->tasklet_head = 0;
    state->tasklet_tail = 0;

    while (tasklet)
    {
        next = tasklet->next;
        tasklet->next = 0;
        tasklet->scheduled = false;
        if (tasklet->func)
        {
            tasklet->func(tasklet->data);
        }
        tasklet = next;
    }
}

void softirq_init(void)
{
    memset((int8_t *)softirq_state, 0, sizeof(softirq_state));
    memset((int8_t *)softirq_vec, 0, sizeof(softirq_vec));
    softirq_vec[TASKLET_SOFTIRQ] = tasklet_softirq_action;
}

void softirq_init_secondary(uint32_t cpu_id)
{
    if (cpu_id >= CONFIG_MAX_CPUS)
    {
        return;
    }

    memset((int8_t *)&softirq_state[cpu_id], 0, sizeof(softirq_state[cpu_id]));
}

void raise_softirq(uint32_t nr)
{
    uint32_t cpu_id;
    uint64_t daif;

    if (nr >= NR_SOFTIRQS)
    {
        return;
    }

    cpu_id = smp_cpu_id();
    if (cpu_id >= CONFIG_MAX_CPUS)
    {
        return;
    }

    daif = read_daif();
    disable_irq();
    softirq_state[cpu_id].pending |= (1U << nr);
    write_daif(daif);
}

static void __do_softirq(uint32_t cpu_id)
{
    struct softirq_cpu_state *state;

    state = &softirq_state[cpu_id];
    if (state->in_softirq)
    {
        return;
    }

    state->in_softirq = true;
    while (state->pending)
    {
        uint32_t pending;
        uint32_t nr;

        pending = state->pending;
        state->pending = 0;

        for (nr = 0; nr < NR_SOFTIRQS; nr++)
        {
            if ((pending & (1U << nr)) == 0)
            {
                continue;
            }

            if (softirq_vec[nr])
            {
                softirq_vec[nr](cpu_id);
            }
        }
    }
    state->in_softirq = false;
}

void softirq_irq_exit(void)
{
    uint32_t cpu_id;

    cpu_id = smp_cpu_id();
    if (cpu_id >= CONFIG_MAX_CPUS)
    {
        return;
    }

    __do_softirq(cpu_id);
}

uint32_t softirq_pending_mask(uint32_t cpu_id)
{
    if (cpu_id >= CONFIG_MAX_CPUS)
    {
        return 0;
    }

    return softirq_state[cpu_id].pending;
}

void tasklet_init(struct tasklet_struct *tasklet, tasklet_func_t func, unsigned long data)
{
    if (!tasklet)
    {
        return;
    }

    tasklet->next = 0;
    tasklet->func = func;
    tasklet->data = data;
    tasklet->scheduled = false;
}

void tasklet_schedule(struct tasklet_struct *tasklet)
{
    uint32_t cpu_id;
    uint64_t daif;
    struct softirq_cpu_state *state;

    if (!tasklet)
    {
        return;
    }

    cpu_id = smp_cpu_id();
    if (cpu_id >= CONFIG_MAX_CPUS)
    {
        return;
    }

    daif = read_daif();
    disable_irq();

    if (tasklet->scheduled)
    {
        write_daif(daif);
        return;
    }

    state = &softirq_state[cpu_id];
    tasklet->scheduled = true;
    tasklet->next = 0;

    if (!state->tasklet_head)
    {
        state->tasklet_head = tasklet;
        state->tasklet_tail = tasklet;
    }
    else
    {
        state->tasklet_tail->next = tasklet;
        state->tasklet_tail = tasklet;
    }

    state->pending |= (1U << TASKLET_SOFTIRQ);
    write_daif(daif);
}
