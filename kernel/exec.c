#include "exec.h"

#include "elf.h"
#include "errno.h"
#include "fs/vfs.h"
#include "lib/libmem.h"
#include "lib/libstr.h"
#include "mm/mm.h"
#include "mm/page_alloc.h"
#include "sched.h"

struct exec_image
{
    void *file_buf;
    uint32_t file_order;
    void *image_base;
    uint32_t image_order;
    int argc;
    int (*entry)(int argc, char **argv);
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
    uint64_t stack_top;

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

    task_set_cleanup(exec_cleanup, image);
    stack_top = (uint64_t)&task->stack[TASK_STACK_SIZE];
    /*
     * 不再把 ELF 入口当成普通 C 函数直接调用。
     * 这里通过汇编跳板切到当前 task 的干净栈顶，并把 x30 固定成
     * exec_return_from_entry()，避免返回地址继续依赖旧的 C 栈帧布局。
     */
    exec_enter(image->entry, image->argc, image->argv, stack_top, exec_return_from_entry);
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
        task->has_exec_image = true;
    }

    return ret;
}
