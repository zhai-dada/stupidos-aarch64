#include "shell.h"

#include "errno.h"
#include "driver/virtio_input.h"
#include "driver/virtio_net.h"
#include "exec.h"
#include "lib/libasm.h"
#include "tty.h"

static int32_t shell_pid = -1;
static bool shell_supervisor_started;

int32_t shell_foreground_pid(void)
{
    return shell_pid;
}
#include "fs/vfs.h"
#include "lib/libmem.h"
#include "lib/libstr.h"
#include "mm/mm.h"
#include "mm/page_alloc.h"
#include "net/net.h"
#include "printk.h"
#include "sched.h"
#include "smp.h"
#include "softirq.h"
#include "timer.h"

#define SHELL_LINE_MAX 256
#define SHELL_READ_CHUNK 128
#define SHELL_MAX_ARGS 8

static void shell_puts(const int8_t *str)
{
    tty_write(str);
}

static void shell_write_bytes(const int8_t *buf, size_t len)
{
    tty_write_bytes(buf, len);
}

static int shell_launch_userspace(void)
{
    int ret;
    const int8_t *argv[] = {(const int8_t *)"sh", 0};

    ret = exec_program((const int8_t *)"/bin/sh", 1, argv);
    if (ret < 0)
    {
        return ret;
    }

    shell_pid = ret;
    return ret;
}

static bool shell_is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static int shell_tokenize(int8_t *line, int8_t *argv[], int max_args)
{
    int argc;
    int8_t *p;

    argc = 0;
    p = line;
    while (*p != '\0')
    {
        while (*p != '\0' && shell_is_space((char)*p))
        {
            *p++ = '\0';
        }

        if (*p == '\0')
        {
            break;
        }

        if (argc >= max_args)
        {
            return -E2BIG;
        }

        argv[argc++] = p;
        while (*p != '\0' && !shell_is_space((char)*p))
        {
            p++;
        }
    }

    return argc;
}

static void shell_print_error(int ret)
{
    printk("[shell\t]: error %d\n", ret);
}

static void shell_help(void)
{
    shell_puts((const int8_t *)"\r\ncommands:\r\n");
    shell_puts((const int8_t *)"  help            - show this message\r\n");
    shell_puts((const int8_t *)"  run <path> ...  - run an ELF program from VFS\r\n");
    shell_puts((const int8_t *)"  ls [path]       - list directory entries\r\n");
    shell_puts((const int8_t *)"  cat <path>      - dump a file\r\n");
    shell_puts((const int8_t *)"  write <path> <text> - overwrite a file from offset 0\r\n");
    shell_puts((const int8_t *)"  info            - show kernel status\r\n");
    shell_puts((const int8_t *)"  ps              - show task table\r\n");
    shell_puts((const int8_t *)"  net             - show network devices\r\n");
    shell_puts((const int8_t *)"  ping [args]     - execute /bin/ping\r\n");
    shell_puts((const int8_t *)"  nettest         - compatibility alias for /bin/ping\r\n");
    shell_puts((const int8_t *)"  mem             - show buddy allocator state\r\n");
    shell_puts((const int8_t *)"  external cmds   - /bin/hello /bin/ls /bin/cat /bin/ping\r\n");
}

static void shell_ls(const int8_t *path)
{
    struct vfs_dirent ent;
    uint32_t index;
    int ret;

    shell_puts((const int8_t *)"\r\n");
    shell_puts((const int8_t *)path);
    shell_puts((const int8_t *)":\r\n");

    for (index = 0; ; index++)
    {
        ret = vfs_readdir(path, index, &ent);
        if (ret == -ENOENT)
        {
            break;
        }
        if (ret)
        {
            shell_print_error(ret);
            return;
        }

        shell_puts((const int8_t *)((ent.mode & VFS_S_IFMT) == VFS_S_IFDIR ? "[d] " : "[-] "));
        shell_puts((const int8_t *)ent.name);
        shell_puts((const int8_t *)"\r\n");
    }
}

static void shell_cat(const int8_t *path)
{
    int fd;
    ssize_t nread;
    int8_t buf[SHELL_READ_CHUNK];

    fd = vfs_open(path, VFS_O_RDONLY);
    if (fd < 0)
    {
        shell_print_error(fd);
        return;
    }

    shell_puts((const int8_t *)"\r\n");
    while (1)
    {
        nread = vfs_read(fd, buf, sizeof(buf));
        if (nread < 0)
        {
            shell_print_error((int)nread);
            break;
        }

        if (nread == 0)
        {
            break;
        }

        shell_write_bytes(buf, (size_t)nread);
    }
    shell_puts((const int8_t *)"\r\n");
    vfs_close(fd);
}

static void shell_write(const int8_t *path, const char *text)
{
    int fd;
    ssize_t nwrite;
    size_t len;

    fd = vfs_open(path, VFS_O_RDWR);
    if (fd < 0)
    {
        shell_print_error(fd);
        return;
    }

    len = strlen((int8_t *)text);
    if (vfs_lseek(fd, 0, VFS_SEEK_SET) < 0)
    {
        shell_puts((const int8_t *)"seek failed\r\n");
        vfs_close(fd);
        return;
    }

    nwrite = vfs_write(fd, text, len);
    if (nwrite < 0)
    {
        shell_print_error((int)nwrite);
        vfs_close(fd);
        return;
    }

    shell_puts((const int8_t *)"written: ");
    shell_write_bytes((const int8_t *)text, (size_t)nwrite);
    shell_puts((const int8_t *)"\r\n");
    vfs_close(fd);
}

static void shell_info(void)
{
    printk("[shell\t]: jiffies=%lx cpus=%u online=%u current=%d softirq=%#x netirq=%u inputirq=%u\n",
           jiffies, smp_cpu_count(), smp_online_count(),
           task_current() ? task_current()->pid : -1,
           softirq_pending_mask(smp_cpu_id()),
           virtio_net_irq_count(),
           virtio_input_irq_count());
}

static void shell_mem(void)
{
    printk("[mem\t]: free=%u pages total=%u pages (%lu MB total)\n",
           page_alloc_free_pages(), page_alloc_total_pages(),
           (uint64_t)(TOTAL_MEMORY / 0x100000));
}

static void shell_supervisor(void *arg)
{
    struct task_struct *task;
    int ret;

    (void)arg;
    while (1)
    {
        task = task_by_pid(shell_pid);
        if (!task || task->state == TASK_DEAD)
        {
            ret = shell_launch_userspace();
            if (ret < 0)
            {
                printk("[shell\tinit]: relaunch failed %d\n", ret);
            }
        }

        sched_maybe_resched();
        /*
         * 这里是“等 shell 退出 / 重启”的软件等待，不是纯粹省电。
         * 用 wfe 才能被 task_exit() 里的 sev() 立刻唤醒。
         */
        wfe();
    }
}

static int shell_exec_external(int argc, int8_t *argv[], bool direct_path_only)
{
    int8_t path[VFS_PATH_MAX];
    int ret;
    size_t base_len;

    if (argc <= 0 || !argv || !argv[0] || argv[0][0] == '\0')
    {
        return -EINVAL;
    }

    if (argv[0][0] == '/')
    {
        return exec_program(argv[0], argc, (const int8_t **)argv);
    }

    if (direct_path_only)
    {
        return -ENOENT;
    }

    memset((int8_t *)path, 0, sizeof(path));
    memcpy(path, (int8_t *)"/bin/", 5);
    base_len = strlen((int8_t *)argv[0]);
    if (5 + base_len + 1 > sizeof(path))
    {
        return -ENAMETOOLONG;
    }
    memcpy(path + 5, argv[0], base_len + 1);
    ret = exec_program(path, argc, (const int8_t **)argv);
    return ret;
}

static void shell_execute(int8_t *line)
{
    int8_t *argv[SHELL_MAX_ARGS];
    int argc;
    int ret;

    argc = shell_tokenize(line, argv, SHELL_MAX_ARGS);
    if (argc == 0)
    {
        return;
    }
    if (argc < 0)
    {
        shell_print_error(argc);
        return;
    }

    if (strcmp(argv[0], (const int8_t *)"help") == 0)
    {
        shell_help();
        return;
    }

    if (strcmp(argv[0], (const int8_t *)"run") == 0)
    {
        if (argc < 2)
        {
            shell_puts((const int8_t *)"usage: run <path> [args...]\r\n");
            return;
        }

        ret = shell_exec_external(argc - 1, &argv[1], true);
        if (ret < 0)
        {
            shell_print_error(ret);
            return;
        }
        return;
    }

    if (strcmp(argv[0], (const int8_t *)"ls") == 0)
    {
        if (argc >= 2)
        {
            shell_ls(argv[1]);
        }
        else
        {
            shell_ls((const int8_t *)"/");
        }
        return;
    }

    if (strcmp(argv[0], (const int8_t *)"cat") == 0)
    {
        if (argc < 2)
        {
            shell_puts((const int8_t *)"usage: cat <path>\r\n");
            return;
        }

        shell_cat(argv[1]);
        return;
    }

    if (strcmp(argv[0], (const int8_t *)"write") == 0)
    {
        int8_t *text;

        if (argc < 3)
        {
            shell_puts((const int8_t *)"usage: write <path> <text>\r\n");
            return;
        }

        text = argv[2];
        shell_write(argv[1], (const char *)text);
        return;
    }

    if (strcmp(argv[0], (const int8_t *)"info") == 0)
    {
        shell_info();
        return;
    }

    if (strcmp(argv[0], (const int8_t *)"ps") == 0)
    {
        sched_show_tasks();
        return;
    }

    if (strcmp(argv[0], (const int8_t *)"mem") == 0)
    {
        shell_mem();
        return;
    }

    if (strcmp(argv[0], (const int8_t *)"net") == 0)
    {
        net_show_status();
        return;
    }

    if (strcmp(argv[0], (const int8_t *)"ping") == 0)
    {
        ret = shell_exec_external(argc, argv, false);
        if (ret < 0)
        {
            shell_print_error(ret);
        }
        return;
    }

    if (strcmp(argv[0], (const int8_t *)"nettest") == 0)
    {
        int8_t *alias_argv[SHELL_MAX_ARGS];
        size_t i;

        if (argc > SHELL_MAX_ARGS)
        {
            shell_print_error(-E2BIG);
            return;
        }

        alias_argv[0] = (int8_t *)"ping";
        for (i = 1; i < (size_t)argc; i++)
        {
            alias_argv[i] = argv[i];
        }

        ret = shell_exec_external(argc, alias_argv, false);
        if (ret < 0)
        {
            shell_print_error(ret);
        }
        return;
    }

    ret = shell_exec_external(argc, argv, false);
    if (ret >= 0)
    {
        return;
    }

    shell_puts((const int8_t *)"unknown command\r\n");
}

static void shell_main(void *arg)
{
    int8_t line[SHELL_LINE_MAX];
    size_t len;
    int ch;

    (void)arg;
    shell_puts((const int8_t *)"\r\nstupidos shell\r\n");
    shell_help();

    while (1)
    {
        len = 0;
        shell_puts((const int8_t *)"stupidos> ");

	        while (1)
	        {
	            ch = tty_getc();
            if (ch == '\r' || ch == '\n')
            {
                line[len] = '\0';
                shell_execute(line);
                break;
            }

            if (ch == '\b' || ch == 0x7f)
            {
                if (len > 0)
                {
                    len--;
                }
                continue;
            }

            if (ch < 0x20 || ch > 0x7e)
            {
                continue;
            }

            if (len + 1 >= sizeof(line))
            {
                continue;
            }

            line[len++] = (int8_t)ch;
        }
    }
}

void shell_init(void)
{
    int ret;

    /*
     * 先清掉启动阶段残留的输入字节，避免 shell 一上线就把脏数据当命令执行。
     */
    tty_flush_input();

    ret = shell_launch_userspace();
    if (ret < 0)
    {
        printk("[shell\tinit]: exec /bin/sh failed %d, fallback to kernel shell\n", ret);
        ret = kthread_create((const int8_t *)"shell", shell_main, 0);
        if (ret < 0)
        {
            printk("[shell\tinit]: failed %d\n", ret);
            return;
        }

        printk("[shell\tinit]: legacy shell started pid=%d\n", ret);
        return;
    }

    if (!shell_supervisor_started)
    {
        shell_supervisor_started = true;
        ret = kthread_create((const int8_t *)"sh-watch", shell_supervisor, 0);
        if (ret < 0)
        {
            printk("[shell\tinit]: supervisor create failed %d\n", ret);
        }
    }
}
