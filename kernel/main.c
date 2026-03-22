#include "driver/uart.h"
#include "asm/sysreg.h"
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
#include "softirq.h"

extern uint8_t framebuffer[FB_WIDTH * FB_HEIGHT * FB_BPP];

extern uint64_t __bss_start, __bss_end;
extern uint64_t __kernel_start, __kernel_end;

uint64_t boot_dtb_phys;

static void debug_irq_snapshot(void)
{
    uint32_t spin;
    uint64_t cntp_ctl;
    uint64_t cntp_tval;
    uint32_t ppi_pending;
    uint32_t spi_pending;
    uint32_t hppir;
    uint32_t ahppir;
    uint32_t gicc_ctlr;
    uint32_t gicd_ctlr;

    for (spin = 0; spin < 25000000U; spin++)
    {
        nop();
    }

    asm volatile("mrs %0, cntp_ctl_el0" : "=r"(cntp_ctl) : : "memory");
    asm volatile("mrs %0, cntp_tval_el0" : "=r"(cntp_tval) : : "memory");
    ppi_pending = get32(GICD_ISPENDR + 0);
    spi_pending = get32(GICD_ISPENDR + 4);
    hppir = get32(GICC_HPPIR) & 0x3ffU;
    ahppir = get32(GICC_AHPPIR) & 0x3ffU;
    gicc_ctlr = get32(GICC_CTLR);
    gicd_ctlr = get32(GICD_CTLR);

    printk("[irq\tdebug]: daif=%#lx jiffies=%lu cntp_ctl=%#lx cntp_tval=%ld ppi_pend=%#x spi_pend=%#x gicd_ctlr=%#x gicc_ctlr=%#x hppir=%u ahppir=%u\n",
           read_daif(), jiffies, cntp_ctl, (int64_t)(int32_t)cntp_tval,
           ppi_pending, spi_pending, gicd_ctlr, gicc_ctlr, hppir, ahppir);
}

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
    enable_irq();

    /*
     * 当前启动路径下仍然永久保留 TTBR0 的低地址 idmap，
     * 所以异常向量先继续留在低地址 vectors。
     *
     * 实测把 VBAR_EL1 切到 KIMAGE_VADDR 别名后，IRQ 会停止进入。
     * 在彻底查清高地址向量页的问题前，先维持低地址向量，
     * 保证 timer/UART 中断和 shell 交互稳定可用。
     */
    printk("[mmu\tinit]: now running at KIMAGE_VADDR vbar=%#lx daif=%#lx jiffies=%lu\n",
           read_sysreg(vbar_el1), read_daif(), jiffies);
    fdt_log_summary();

    /*
     * 先把调度器和 shell 拉起来。
     * shell 等待输入时会主动 yield，不会再像之前那样把 boot 线程卡死，
     * 这样后面的文件系统、PCI、输入和网络初始化可以继续在后台推进。
     */
    sched_init();
    smp_init();
    printk("[boot\tinit]: shell start\n");
    shell_init();
    if (virtio_input_init())
    {
        printk("[input\tinit]: virtio-input init failed\n");
    }
    sched_yield();

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

    page_alloc_init();
    syscall_init();

    pci_init();
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

    printk("[boot\tinit]: ui dashboard start\n");
    ui_boot_screen();
    printk("[boot\tinit]: ui dashboard done\n");

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

    while (1)
    {
        sched_maybe_resched();
    }
}

int32_t kernel_main(uint64_t dtb_phys)
{
    uint64_t current_el;

    /* 这里按字节清零 .bss，不能按 uint64_t 个数来减。 */
    memset((int8_t *)&__bss_start, 0, (uint64_t)&__bss_end - (uint64_t)&__bss_start);

    early_uart_init();
    current_el = read_sysreg(CurrentEL);
    printk("[boot\tinit]: CurrentEL=%lu\n", current_el >> 2);
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
    softirq_init();

    timer_init();

    enable_irq();
    printk("[boot\tinit]: DAIF=%#lx after enable_irq\n", read_daif());
    debug_irq_snapshot();

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
     * 从这里开始要同时迁移执行地址、栈和异常向量基址。
     * 如果这个窗口里放开 IRQ，定时器可能打在“PC 已经切高地址、
     * 但 VBAR 还没切过去”的过渡期，调试上会非常混乱。
     */
    disable_irq();

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
