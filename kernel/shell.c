#include "shell.h"

#include "errno.h"
#include "driver/virtio_input.h"
#include "driver/virtio_net.h"
#include "tty.h"
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

static void shell_putc(char ch)
{
    tty_putc((uint8_t)ch);
}

static void shell_puts(const int8_t *str)
{
    tty_write(str);
}

static void shell_write_bytes(const int8_t *buf, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        shell_putc((char)buf[i]);
    }
}

static bool shell_is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static const char *shell_skip_spaces(const char *p)
{
    while (*p && shell_is_space(*p))
    {
        p++;
    }

    return p;
}

static int shell_next_token(const char **cursor, int8_t *out, size_t out_len)
{
    const char *p;
    size_t len;

    if (!cursor || !out || !out_len)
    {
        return -EINVAL;
    }

    p = shell_skip_spaces(*cursor);
    if (*p == '\0')
    {
        *cursor = p;
        return -ENOENT;
    }

    len = 0;
    while (*p && !shell_is_space(*p))
    {
        if (len + 1 >= out_len)
        {
            return -ENAMETOOLONG;
        }

        out[len++] = (int8_t)*p++;
    }
    out[len] = '\0';
    *cursor = p;
    return 0;
}

static void shell_print_error(int ret)
{
    printk("[shell\t]: error %d\n", ret);
}

static void shell_help(void)
{
    shell_puts((const int8_t *)"\r\ncommands:\r\n");
    shell_puts((const int8_t *)"  help            - show this message\r\n");
    shell_puts((const int8_t *)"  ls [path]       - list directory entries\r\n");
    shell_puts((const int8_t *)"  cat <path>      - dump a file\r\n");
    shell_puts((const int8_t *)"  write <path> <text> - overwrite a file from offset 0\r\n");
    shell_puts((const int8_t *)"  info            - show kernel status\r\n");
    shell_puts((const int8_t *)"  ps              - show task table\r\n");
    shell_puts((const int8_t *)"  net             - show network devices\r\n");
    shell_puts((const int8_t *)"  ping/nettest    - run network self-test\r\n");
    shell_puts((const int8_t *)"  mem             - show buddy allocator state\r\n");
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

        shell_puts((const int8_t *)(ent.mode == VFS_S_IFDIR ? "[d] " : "[-] "));
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

static void shell_ping(void)
{
    int ret;

    ret = net_selftest();
    if (ret)
    {
        printk("[shell\t]: network selftest failed %d\n", ret);
        return;
    }

    printk("[shell\t]: network selftest passed\n");
}

static void shell_execute(const int8_t *line)
{
    const char *cursor;
    int8_t cmd[32];
    int8_t path[VFS_PATH_MAX];
    int ret;

    cursor = (const char *)line;
    cursor = shell_skip_spaces(cursor);
    if (*cursor == '\0')
    {
        return;
    }

    ret = shell_next_token(&cursor, cmd, sizeof(cmd));
    if (ret)
    {
        shell_print_error(ret);
        return;
    }

    if (strcmp(cmd, (const int8_t *)"help") == 0)
    {
        shell_help();
        return;
    }

    if (strcmp(cmd, (const int8_t *)"ls") == 0)
    {
        ret = shell_next_token(&cursor, path, sizeof(path));
        if (ret == -ENOENT)
        {
            path[0] = '/';
            path[1] = '\0';
        }
        else if (ret)
        {
            shell_print_error(ret);
            return;
        }

        shell_ls(path);
        return;
    }

    if (strcmp(cmd, (const int8_t *)"cat") == 0)
    {
        ret = shell_next_token(&cursor, path, sizeof(path));
        if (ret)
        {
            shell_print_error(ret);
            return;
        }

        shell_cat(path);
        return;
    }

    if (strcmp(cmd, (const int8_t *)"write") == 0)
    {
        const char *text;

        ret = shell_next_token(&cursor, path, sizeof(path));
        if (ret)
        {
            shell_print_error(ret);
            return;
        }

        text = shell_skip_spaces(cursor);
        if (*text == '\0')
        {
            shell_puts((const int8_t *)"missing text\r\n");
            return;
        }

        shell_write(path, text);
        return;
    }

    if (strcmp(cmd, (const int8_t *)"info") == 0)
    {
        shell_info();
        return;
    }

    if (strcmp(cmd, (const int8_t *)"ps") == 0)
    {
        sched_show_tasks();
        return;
    }

    if (strcmp(cmd, (const int8_t *)"mem") == 0)
    {
        shell_mem();
        return;
    }

    if (strcmp(cmd, (const int8_t *)"net") == 0)
    {
        net_show_status();
        return;
    }

    if (strcmp(cmd, (const int8_t *)"ping") == 0 || strcmp(cmd, (const int8_t *)"nettest") == 0)
    {
        shell_ping();
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
                shell_puts((const int8_t *)"\r\n");
                line[len] = '\0';
                shell_execute(line);
                break;
            }

            if (ch == '\b' || ch == 0x7f)
            {
                if (len > 0)
                {
                    len--;
                    shell_puts((const int8_t *)"\b \b");
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
            shell_putc((char)ch);
        }
    }
}

void shell_init(void)
{
    int ret;

    ret = kthread_create((const int8_t *)"shell", shell_main, 0);
    if (ret < 0)
    {
        printk("[shell\tinit]: failed %d\n", ret);
        return;
    }

    printk("[shell\tinit]: started pid=%d\n", ret);
}
