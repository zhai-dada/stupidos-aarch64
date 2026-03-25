#include "exec.h"

#include "elf.h"
#include "errno.h"
#include "fs/vfs.h"
#include "lib/libmem.h"
#include "lib/libstr.h"
#include "mm/mm.h"
#include "mm/page_alloc.h"
#include "mmu.h"
#include "sched.h"

struct exec_image
{
    void *file_buf;
    uint32_t file_order;
    void *image_base;
    uint32_t image_order;
    int argc;
    int (*entry)(int argc, char **argv);
    bool has_alias;
    uint64_t alias_base;
    uint64_t alias_end;
    int8_t path[VFS_PATH_MAX];
    int8_t arg_buf[EXEC_ARG_BUF_SIZE];
    char *argv[EXEC_MAX_ARGS + 1];
};

extern void exec_enter(int (*entry)(int argc, char **argv),
                       int argc,
                       char **argv,
                       uint64_t stack_top,
                       void (*return_handler)(int ret));

static uint64_t exec_align_up(uint64_t value, uint64_t align)
{
    if (!align)
    {
        return value;
    }

    return (value + align - 1) & ~(align - 1);
}

static uint32_t exec_order_for_size(uint64_t size)
{
    uint32_t order;
    uint64_t bytes;

    order = 0;
    bytes = PAGE_SIZE;
    while (bytes < size && order < PAGE_ALLOC_MAX_ORDER)
    {
        order++;
        bytes <<= 1;
    }

    return order;
}

static void exec_sync_icache(void *base, uint64_t size)
{
    uint64_t start;
    uint64_t end;
    uint64_t addr;

    if (!base || !size)
    {
        return;
    }

    /*
     * AArch64 关键点（中文）：
     * 内核用 memcpy 把 ELF 指令写到新页后，I-Cache 不会自动和 D-Cache 保持一致。
     * 如果不做显式同步，CPU 可能继续执行该物理页上“旧任务残留”的指令行，
     * 表现为用户程序随机跳转、偶发指令异常（本项目里正好命中 mkdir/ls 路径）。
     */
    start = (uint64_t)base & ~63ULL;
    end = ((uint64_t)base + size + 63ULL) & ~63ULL;

    for (addr = start; addr < end; addr += 64ULL)
    {
        asm volatile("dc cvau, %0" : : "r"(addr) : "memory");
    }
    dsb(ish);

    for (addr = start; addr < end; addr += 64ULL)
    {
        asm volatile("ic ivau, %0" : : "r"(addr) : "memory");
    }
    dsb(ish);
    isb();
}

static const int8_t *exec_task_name_from_path(const int8_t *path)
{
    const int8_t *name;
    size_t i;

    name = path;
    for (i = 0; path && path[i] != '\0'; i++)
    {
        if (path[i] == '/')
        {
            name = &path[i + 1];
        }
    }

    if (!name || name[0] == '\0')
    {
        return (const int8_t *)"user";
    }

    return name;
}

static void exec_cleanup(void *arg)
{
    struct exec_image *image;

    image = (struct exec_image *)arg;
    if (!image)
    {
        return;
    }

    if (image->image_base)
    {
        free_pages(image->image_base, image->image_order);
    }

    if (image->file_buf)
    {
        free_pages(image->file_buf, image->file_order);
    }

    free_pages(image, 0);
}

static void exec_return_from_entry(int ret) __attribute__((noreturn));

static void exec_return_from_entry(int ret)
{
    (void)ret;
    task_exit();
}

static void exec_task_main(void *arg)
{
    struct exec_image *image;
    struct task_struct *task;
    uint64_t stack_low;
    uint64_t live_sp;
    uint64_t user_sp;
    char *argv_user[EXEC_MAX_ARGS + 1];
    char **argv_ptr;
    int i;

    image = (struct exec_image *)arg;
    if (!image || !image->entry)
    {
        task_exit();
    }

    task = task_current();
    if (!task)
    {
        task_exit();
    }

    /*
     * 这里在“新任务真正开始执行”的第一时间标记 exec 镜像范围。
     * 避免 kthread_create() 返回到父任务后，子任务已经先跑起来导致的竞态：
     * 竞态窗口里若发生 fault，会被误判成非 exec 任务。
     */
    task->exec_base = (uint64_t)image->image_base;
    task->exec_end = (uint64_t)image->image_base + ((uint64_t)PAGE_SIZE << image->image_order);
    task->exec_alias_base = image->has_alias ? image->alias_base : 0;
    task->exec_alias_end = image->has_alias ? image->alias_end : 0;
    task->has_exec_image = true;

    task_set_cleanup(exec_cleanup, image);
    stack_low = (uint64_t)&task->stack[0];
    asm volatile("mov %0, sp" : "=r"(live_sp) : : "memory");

    /*
     * 关键修复（中文）：
     * 不能从 stack_top 往下直接写 argv。
     *
     * exec_task_main() 自己正在这个 task 栈上运行，当前活动栈帧就位于
     * [live_sp, stack_top) 区间。若把参数字符串塞到这块区域，会把当前
     * 栈帧（包括保存的 fp/lr）踩坏，随后在 exec_enter/异常回溯里表现为
     * 返回地址随机污染（之前观测到 lr 高位被 '/bin/..' 字节覆盖）。
     *
     * 正确做法是：从“当前 live_sp 再往下预留一段保护带”开始打包 argv，
     * 确保整个参数块都落在 live_sp 以下，完全避开活动调用栈。
     */
    if (live_sp < stack_low + 512U)
    {
        task_exit();
    }

    user_sp = live_sp - 256U;
    for (i = image->argc - 1; i >= 0; i--)
    {
        size_t len;

        len = strlen((int8_t *)image->argv[i]) + 1U;
        if (user_sp < stack_low + len + 64U)
        {
            task_exit();
        }

        user_sp -= len;
        memcpy((int8_t *)user_sp, (int8_t *)image->argv[i], len);
        argv_user[i] = (char *)user_sp;
    }
    argv_user[image->argc] = 0;

    user_sp &= ~0xfULL;
    if (user_sp < stack_low + ((uint64_t)(image->argc + 1) * sizeof(char *)) + 16U)
    {
        task_exit();
    }

    user_sp -= (uint64_t)(image->argc + 1) * sizeof(char *);
    argv_ptr = (char **)user_sp;
    for (i = 0; i < image->argc; i++)
    {
        argv_ptr[i] = argv_user[i];
    }
    argv_ptr[image->argc] = 0;

    /*
     * 不再把 ELF 入口当成普通 C 函数直接调用。
     * 这里通过汇编跳板切到当前 task 的干净栈顶，并把 x30 固定成
     * exec_return_from_entry()，避免返回地址继续依赖旧的 C 栈帧布局。
     */
    exec_enter(image->entry, image->argc, argv_ptr, user_sp, exec_return_from_entry);
    __builtin_unreachable();
}

int exec_program(const int8_t *path, int argc, const int8_t *argv[])
{
    elf64_ehdr_t *ehdr;
    elf64_phdr_t *phdr;
    struct exec_image *image;
    uint64_t min_vaddr;
    uint64_t max_vaddr;
    uint64_t image_span;
    uint64_t load_base;
    int fd;
    int ret;
    int64_t file_size;
    ssize_t nread;
    uint32_t i;
    uint32_t file_order;
    uint32_t image_order;
    size_t arg_off;
    struct task_struct *task;

    if (!page_alloc_is_ready())
    {
        return -EAGAIN;
    }

    if (!path || argc <= 0 || argc > EXEC_MAX_ARGS || !argv)
    {
        return -EINVAL;
    }


    fd = vfs_open(path, VFS_O_RDONLY);
    if (fd < 0)
    {
        return fd;
    }

    file_size = vfs_file_size(fd);
    if (file_size <= 0)
    {
        vfs_close(fd);
        return file_size < 0 ? (int)file_size : -ENOEXEC;
    }

    file_order = exec_order_for_size((uint64_t)file_size);
    if (((uint64_t)PAGE_SIZE << file_order) < (uint64_t)file_size)
    {
        vfs_close(fd);
        return -EFBIG;
    }

    image = (struct exec_image *)alloc_pages(0);
    if (!image)
    {
        vfs_close(fd);
        return -ENOMEM;
    }
    memset((int8_t *)image, 0, PAGE_SIZE);

    image->file_buf = alloc_pages(file_order);
    if (!image->file_buf)
    {
        free_pages(image, 0);
        vfs_close(fd);
        return -ENOMEM;
    }
    image->file_order = file_order;
    memset((int8_t *)image->file_buf, 0, (size_t)((uint64_t)PAGE_SIZE << file_order));

    nread = vfs_read(fd, image->file_buf, (size_t)file_size);
    vfs_close(fd);
    if (nread != file_size)
    {
        exec_cleanup(image);
        return nread < 0 ? (int)nread : -EIO;
    }

    ehdr = (elf64_ehdr_t *)image->file_buf;
    if (file_size < (int64_t)sizeof(*ehdr) ||
        ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
        ehdr->e_ident[EI_VERSION] != EV_CURRENT ||
        (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) ||
        ehdr->e_machine != EM_AARCH64 ||
        ehdr->e_phentsize != sizeof(elf64_phdr_t))
    {
        exec_cleanup(image);
        return -ENOEXEC;
    }

    if (!ehdr->e_phoff || !ehdr->e_phnum ||
        ehdr->e_phoff + ((uint64_t)ehdr->e_phnum * sizeof(elf64_phdr_t)) > (uint64_t)file_size)
    {
        exec_cleanup(image);
        return -ENOEXEC;
    }

    phdr = (elf64_phdr_t *)((uint8_t *)image->file_buf + ehdr->e_phoff);
    min_vaddr = (uint64_t)-1;
    max_vaddr = 0;
    for (i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdr[i].p_type != PT_LOAD)
        {
            continue;
        }

        if (phdr[i].p_offset + phdr[i].p_filesz > (uint64_t)file_size || phdr[i].p_memsz < phdr[i].p_filesz)
        {
            exec_cleanup(image);
            return -ENOEXEC;
        }

        if (phdr[i].p_vaddr < min_vaddr)
        {
            min_vaddr = phdr[i].p_vaddr;
        }

        if (phdr[i].p_vaddr + phdr[i].p_memsz > max_vaddr)
        {
            max_vaddr = phdr[i].p_vaddr + phdr[i].p_memsz;
        }
    }

    if (min_vaddr == (uint64_t)-1 || max_vaddr <= min_vaddr)
    {
        exec_cleanup(image);
        return -ENOEXEC;
    }

    image_span = exec_align_up(max_vaddr - min_vaddr, PAGE_SIZE);
    image_order = exec_order_for_size(image_span);
    if (((uint64_t)PAGE_SIZE << image_order) < image_span)
    {
        exec_cleanup(image);
        return -EFBIG;
    }

    image->image_base = alloc_pages(image_order);
    if (!image->image_base)
    {
        exec_cleanup(image);
        return -ENOMEM;
    }
    image->image_order = image_order;
    memset((int8_t *)image->image_base, 0, (size_t)((uint64_t)PAGE_SIZE << image_order));

    load_base = (uint64_t)image->image_base - min_vaddr;
    for (i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdr[i].p_type != PT_LOAD)
        {
            continue;
        }

        memcpy((int8_t *)(load_base + phdr[i].p_vaddr),
               (int8_t *)image->file_buf + phdr[i].p_offset,
               (size_t)phdr[i].p_filesz);
        if (phdr[i].p_memsz > phdr[i].p_filesz)
        {
            memset((int8_t *)(load_base + phdr[i].p_vaddr + phdr[i].p_filesz),
                   0,
                   (size_t)(phdr[i].p_memsz - phdr[i].p_filesz));
        }
    }

    /*
     * 关键修复（中文）：
     * ELF 指令段写入后，必须在“最终执行地址”对应的页上做 I-Cache 同步。
     * 之前把同步放在了镜像分配前，既没有真实地址也没有真实大小，等于没同步。
     * 这里统一在段拷贝完成后同步整个装载区，避免执行到旧指令行。
     */
    exec_sync_icache(image->image_base, (uint64_t)PAGE_SIZE << image_order);

    if (min_vaddr < PAGE_OFFSET)
    {
        uint64_t alias_va;
        uint64_t alias_size;
        uint64_t alias_pa;

        /*
         * 通用低地址 alias 兼容（中文）：
         * 部分较大静态程序（如 tinycc）在全局函数指针/跳转表里会保留绝对低地址引用，
         * 仅靠 load_base 重定位代码段并不足以覆盖这类数据引用。
         *
         * 这里把 [min_vaddr, max_vaddr) 同一份物理镜像再映射到原始低地址，
         * 让“绝对地址访问”和“高地址入口执行”同时成立。
         *
         * 注意：当前内核尚未做进程独立页表，此 alias 属于全局映射。
         * 后续演进到 per-process mm 时，需要把这里迁移到进程私有地址空间。
         */
        alias_va = PAGE_ALIGN_DOWN(min_vaddr);
        alias_size = exec_align_up(max_vaddr - alias_va, PAGE_SIZE);
        alias_pa = kernel_virt_to_phys((uint64_t)image->image_base + (alias_va - min_vaddr));
        if (mmu_map_low_alias(alias_va, alias_pa, alias_size, PAGE_KERNEL_EXEC))
        {
            exec_cleanup(image);
            return -ENOMEM;
        }
        image->has_alias = true;
        image->alias_base = alias_va;
        image->alias_end = alias_va + alias_size;
    }

    image->argc = argc;
    image->entry = (int (*)(int, char **))(load_base + ehdr->e_entry);
    memset((int8_t *)image->path, 0, sizeof(image->path));
    memcpy((int8_t *)image->path, (int8_t *)path, strlen((int8_t *)path) + 1);

    arg_off = 0;
    for (i = 0; i < (uint32_t)argc; i++)
    {
        size_t len;

        len = strlen((int8_t *)argv[i]) + 1;
        if (arg_off + len > sizeof(image->arg_buf))
        {
            exec_cleanup(image);
            return -E2BIG;
        }

        image->argv[i] = (char *)&image->arg_buf[arg_off];
        memcpy((int8_t *)image->argv[i], (int8_t *)argv[i], len);
    arg_off += len;
    }
    image->argv[argc] = 0;

    ret = kthread_create(exec_task_name_from_path(path), exec_task_main, image);
    if (ret < 0)
    {
        exec_cleanup(image);
        return ret;
    }

    task = task_by_pid(ret);
    if (task)
    {
        /*
         * 让调度器知道这个 task 的 ELF 代码区范围。
         * 后续如果保存/恢复到一个明显跑偏的返回地址，可以直接拒绝切换，
         * 避免把 CPU 送进 framebuffer / bss 这类数据区。
         */
        task->exec_base = (uint64_t)image->image_base;
        task->exec_end = (uint64_t)image->image_base + ((uint64_t)PAGE_SIZE << image->image_order);
        task->exec_alias_base = image->has_alias ? image->alias_base : 0;
        task->exec_alias_end = image->has_alias ? image->alias_end : 0;
        task->has_exec_image = true;
    }

    return ret;
}
