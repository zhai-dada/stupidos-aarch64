#include "driver/uart.h"
#include "lib/libmem.h"
#include "lib/libasm.h"
#include "lib/libirq.h"
#include "debug.h"
#include "printk.h"
#include "timer.h"
#include "gicv2.h"
#include "pt_regs.h"
#include "mmu.h"
#include "mm/page_alloc.h"
#include "assert.h"
#include "driver/ramfb.h"
#include "driver/virtio_input.h"
#include "driver/virtio_net.h"
#include "driver/fwcfg.h"
#include "fdt.h"
#include "fs/ext4.h"
#include "fs/fat32.h"
#include "fs/vfs.h"
#include "net/net.h"
#include "pci.h"
#include "syscall.h"
#include "shell.h"
#include "ui.h"
#include "tty.h"
#include "sched.h"
#include "smp.h"

extern uint8_t framebuffer[FB_WIDTH * FB_HEIGHT * FB_BPP];

extern uint64_t __bss_start, __bss_end;
extern uint64_t __kernel_start, __kernel_end;

uint64_t boot_dtb_phys;

static void demo_worker_a(void *arg)
{
    uint64_t rounds = 0;

    (void)arg;
    while (1)
    {
        rounds++;
        if ((rounds & 0xfff) == 0)
        {
            printk("[sched\tdemo]: worker-a round=%lx\n", rounds);
        }

        /*
         * 这里先保留主动 yield 的演示线程模型。
         * 定时器驱动的抢占式调度会在后续把本路径替换掉，但当前先确保
         * 上下文切换、runqueue 和 vruntime 记账是稳定可验证的。
         */
        sched_yield();
    }
}

static void demo_worker_b(void *arg)
{
    uint64_t rounds = 0;

    (void)arg;
    while (1)
    {
        rounds++;
        if ((rounds & 0xfff) == 0)
        {
            printk("[sched\tdemo]: worker-b round=%lx\n", rounds);
        }

        sched_yield();
    }
}

/*
 * 打开 MMU 后，主动跳到 KIMAGE_VADDR 对应的高地址内核镜像区继续执行。
 * 这样执行地址和 linear map 分离，更接近 Linux arm64 的布局。
 */
static __attribute__((noreturn)) void kernel_main_high(void)
{
    int fd;
    ssize_t nread;
    int8_t file_buf[128];
    write_sysreg(kimage_phys_to_virt((uint64_t)&vectors), vbar_el1);
    isb();

    printk("[mmu\tinit]: now running at KIMAGE_VADDR\n");
    fdt_log_summary();

    ramfb_putstring(COLOR_BLACK, COLOR_WHITE, (uint8_t*)"Hello World\n\btest");
    assert(6 > 5);

    if (ext4_mount_root())
    {
        printk("[fs\tinit]: ext4 mount failed\n");
    }
    else
    {
        fd = vfs_open((int8_t *)"/hello.txt", VFS_O_RDONLY);
        if (fd >= 0)
        {
            nread = vfs_read(fd, file_buf, sizeof(file_buf) - 1);
            if (nread > 0)
            {
                file_buf[nread] = '\0';
                printk("[fs\tread]: /hello.txt => %s\n", file_buf);
            }
            else
            {
                printk("[fs\tread]: /hello.txt read failed %ld\n", nread);
            }
            vfs_close(fd);
        }
        else
        {
            printk("[fs\topen]: /hello.txt failed %d\n", fd);
        }

        if (fat32_mount((const int8_t *)"/boot"))
        {
            printk("[fat32\tinit]: mount /boot failed\n");
        }
        else
        {
            fd = vfs_open((int8_t *)"/boot/readme.txt", VFS_O_RDONLY);
            if (fd >= 0)
            {
                nread = vfs_read(fd, file_buf, sizeof(file_buf) - 1);
                if (nread > 0)
                {
                    file_buf[nread] = '\0';
                    printk("[fat32\tread]: /boot/readme.txt => %s\n", file_buf);
                }
                else
                {
                    printk("[fat32\tread]: /boot/readme.txt read failed %ld\n", nread);
                }
                vfs_close(fd);
            }
            else
            {
                printk("[fat32\topen]: /boot/readme.txt failed %d\n", fd);
            }
        }
    }

    /*
     * 调度器必须先于 SMP 次级核上线完成初始化。
     * 否则次级核本地 timer 打开后，可能在 scheduler 状态尚未准备好时
     * 就进入 tick 路径，造成多核并发访问未初始化的 runqueue。
     */
    sched_init();
    smp_init();
    page_alloc_init();
    syscall_init();

    shell_init();

    pci_init();
    if (virtio_input_init())
    {
        printk("[input\tinit]: virtio-input init failed\n");
    }
    net_init();
    if (virtio_net_init())
    {
        printk("[net\tinit]: virtio-net init failed\n");
    }
    else
    {
        /*
         * 先在启动阶段主动做一次 ARP + ICMP 探测。
         * 这样不用等到人工进 shell，串口日志就能直接告诉我们链路是否通了。
         */
        if (net_selftest())
        {
            printk("[net\tinit]: selftest failed\n");
        }
    }

    ui_boot_screen();

    /*
     * 这一轮先保留最小内核线程框架：
     * - boot 任务作为 pid 0
     * - 创建两个演示内核线程
     * - 通过主动 yield 验证上下文切换链路稳定
     * - 同时继续保留 per-cpu runqueue / vruntime 账本，为后续 CFS 演进打底
     */
    kthread_create((const int8_t *)"worker-a", demo_worker_a, 0);
    kthread_create((const int8_t *)"worker-b", demo_worker_b, 0);
    printk("[sched\tinit]: entering boot idle loop\n");
    sched_yield();

    while (1)
    {
        sched_maybe_resched();
    }
}

int32_t kernel_main(uint64_t dtb_phys)
{
    /* 这里按字节清零 .bss，不能按 uint64_t 个数来减。 */
    memset((int8_t *)&__bss_start, 0, (uint64_t)&__bss_end - (uint64_t)&__bss_start);

    early_uart_init();
    if (!dtb_phys)
    {
        dtb_phys = boot_dtb_phys;
    }
    boot_dtb_phys = dtb_phys;
    printk("[fdt\tinit]: dtb_phys=%#lx\n", dtb_phys);
    fdt_boot_init((const void *)boot_dtb_phys);

    disable_irq();

    mmu_init();
    
    gic_init();

    uart_init();
    tty_init();

    timer_init();

    enable_irq();

    /*
     * 先初始化 framebuffer，再建立页表。
     * framebuffer 本身位于内核 .bss，属于内核镜像映射的一部分。
     */
    ramfb_init((uint8_t *)framebuffer, FB_WIDTH, FB_HEIGHT);

    if (page_map_init())
    {
        printk("[mmu\tinit]: page table build failed\n");
        while (1)
        {
        }
    }

    if (enable_mmu())
    {
        printk("[mmu\tinit]: enable mmu failed\n");
        while (1)
        {
        }
    }

    /*
     * 此时：
     * 1. TTBR0 下仍保留低地址 idmap
     * 2. TTBR1 下已经建立 linear map 和 KIMAGE 映射
     *
     * 主动切换栈和 PC 到 KIMAGE_VADDR 区域，后续内核代码就从独立的
     * 内核镜像映射区运行，而不是继续依赖低地址 identity map。
     */
    asm volatile
    (
        "mov sp, %0\n"
        "blr %1\n"
        :
        : "r"(kimage_phys_to_virt((uint64_t)&init_stack_end)),
          "r"(kimage_phys_to_virt((uint64_t)kernel_main_high))
        : "memory"
    );

    while (1)
    {
    }
}
