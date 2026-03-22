#include "smp.h"
#include "atomic.h"
#include "gicv2.h"
#include "lib/libasm.h"
#include "lib/libirq.h"
#include "lib/libmem.h"
#include "mmu.h"
#include "printk.h"
#include "sched.h"
#include "softirq.h"
#include "timer.h"

#define PSCI_0_2_FN64_CPU_ON   0xC4000003UL
#define PSCI_RET_SUCCESS       0

struct secondary_boot_data
{
    uint64_t stack_phys;
    uint64_t cpu_id;
};

static struct cpu_info cpu_info_table[CONFIG_MAX_CPUS];
static struct secondary_boot_data secondary_boot_data[CONFIG_MAX_CPUS];
static uint8_t secondary_stacks[CONFIG_MAX_CPUS][TASK_STACK_SIZE] __attribute__((aligned(16)));
static atomic_t cpu_online_count = ATOMIC_INIT(0);
static uint32_t cpu_possible_count = 1;

extern void secondary_entry(void);

static int64_t psci_cpu_on(uint64_t target_cpu, uint64_t entry, uint64_t context)
{
    register uint64_t x0 asm("x0") = PSCI_0_2_FN64_CPU_ON;
    register uint64_t x1 asm("x1") = target_cpu;
    register uint64_t x2 asm("x2") = entry;
    register uint64_t x3 asm("x3") = context;

    asm volatile(
        "hvc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3)
        : "x4", "x5", "x6", "x7", "memory"
    );

    return (int64_t)x0;
}

static void secondary_main_high(uint64_t cpu_id) __attribute__((noreturn));

static void secondary_main_high(uint64_t cpu_id)
{
    disable_irq();
    write_sysreg(kimage_phys_to_virt((uint64_t)&vectors), vbar_el1);
    isb();

    gic_init();
    softirq_init_secondary((uint32_t)cpu_id);
    /*
     * 次级核一旦开始接收本地 timer IRQ，就可能进入调度代码。
     * 所以必须先把本 CPU 的 runqueue 状态初始化好，再打开本地 timer。
     */
    sched_init_secondary((uint32_t)cpu_id);
    timer_init_secondary();

    smp_secondary_online((uint32_t)cpu_id);
    printk("[smp\tinit]: cpu %d online, mpidr=%#lx\n", cpu_id, read_mpidr());

    while (1)
    {
        asm volatile("wfi");
    }
}

void smp_secondary_boot(uint64_t cpu_id)
{
    uint64_t high_sp;
    uint64_t high_entry;

    disable_irq();
    mmu_init();
    if (mmu_secondary_init())
    {
        printk("[smp\tinit]: cpu %d failed to enable mmu\n", cpu_id);
        while (1)
        {
            asm volatile("wfi");
        }
    }

    high_sp = kimage_phys_to_virt(secondary_boot_data[cpu_id].stack_phys);
    high_entry = kimage_phys_to_virt((uint64_t)secondary_main_high);

    asm volatile(
        "mov sp, %0\n"
        "mov x0, %1\n"
        "blr %2\n"
        :
        : "r"(high_sp), "r"(cpu_id), "r"(high_entry)
        : "x0", "memory"
    );

    while (1)
    {
        asm volatile("wfi");
    }
}

void smp_secondary_online(uint32_t cpu_id)
{
    cpu_info_table[cpu_id].online = true;
    atomic_inc(&cpu_online_count);
}

uint32_t smp_cpu_id(void)
{
    return arch_curr_cpu_id();
}

uint32_t smp_cpu_count(void)
{
    return cpu_possible_count;
}

uint32_t smp_online_count(void)
{
    return (uint32_t)atomic_read(&cpu_online_count);
}

void smp_init(void)
{
    uint32_t cpu;
    uint32_t max_cpus;

    max_cpus = global_gic_info.cpu_num;
    if (max_cpus == 0)
    {
        max_cpus = 1;
    }
    if (max_cpus > CONFIG_MAX_CPUS)
    {
        max_cpus = CONFIG_MAX_CPUS;
    }

    cpu_possible_count = max_cpus;
    memset((int8_t *)cpu_info_table, 0, sizeof(cpu_info_table));
    memset((int8_t *)secondary_boot_data, 0, sizeof(secondary_boot_data));

    cpu_info_table[0].logical_id = 0;
    cpu_info_table[0].mpidr = read_mpidr();
    cpu_info_table[0].online = true;
    atomic_set(&cpu_online_count, 1);

    printk("[smp\tinit]: boot cpu mpidr=%#lx, target cpus=%d\n",
           cpu_info_table[0].mpidr, cpu_possible_count);

    for (cpu = 1; cpu < cpu_possible_count; cpu++)
    {
        int64_t ret;
        uint64_t stack_top;

        cpu_info_table[cpu].logical_id = cpu;
        cpu_info_table[cpu].mpidr = cpu;

        stack_top = (uint64_t)&secondary_stacks[cpu][TASK_STACK_SIZE];
        stack_top &= ~0xfUL;

        secondary_boot_data[cpu].stack_phys = kernel_virt_to_phys(stack_top);
        secondary_boot_data[cpu].cpu_id = cpu;

        ret = psci_cpu_on(cpu_info_table[cpu].mpidr,
                          kernel_virt_to_phys((uint64_t)secondary_entry),
                          kernel_virt_to_phys((uint64_t)&secondary_boot_data[cpu]));
        if (ret != PSCI_RET_SUCCESS)
        {
            printk("[smp\tinit]: cpu %d cpu_on failed ret=%ld\n", cpu, ret);
            continue;
        }
    }

    /*
     * 这里先做一个简单的忙等，确认次级核已经进入内核。
     * 后续演进到真正 SMP 调度时，会被更完整的 CPU hotplug/同步机制替代。
     */
    for (cpu = 0; cpu < 100000000; cpu++)
    {
        if (smp_online_count() == cpu_possible_count)
        {
            break;
        }
    }

    printk("[smp\tinit]: online cpus=%d/%d\n", smp_online_count(), cpu_possible_count);
}
