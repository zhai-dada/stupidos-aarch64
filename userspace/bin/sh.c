#include "stupidos_user.h"

#define SH_LINE_MAX 256
#define SH_MAX_ARGS  8
#define SH_READ_CHUNK 128

/*
 * shell 运行在一个很低层的环境里，syscall / IRQ / 调度都会反复打断它。
 * 把命令行缓冲和 argv 表放到静态区，可以避免栈帧被异常嵌套时意外踩坏，
 * 这类问题在“用户态仍跑在同一地址空间”阶段尤其容易出现。
 */
static int8_t sh_line_buf[SH_LINE_MAX];
static int8_t sh_arg_storage[SH_MAX_ARGS][SH_LINE_MAX];
static int8_t *sh_argv_buf[SH_MAX_ARGS];
static int8_t sh_cmd_buf[SH_LINE_MAX];

static void sh_puts(const int8_t *str)
{
    /*
     * shell 启动阶段尽量避免把一切都交给不受控的 strlen。
     * 这样即使某个字符串指针被破坏，也只是丢一段输出，不会直接把 shell 撞死。
     */
    u_putsn(str, u_strnlen(str, 4096));
}

static void sh_putc(int8_t ch)
{
    u_putc(ch);
}

static bool sh_is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int sh_parse_line(const int8_t *line, int8_t *argv[], int max_args)
{
    int argc;
    const int8_t *p;
    size_t out;

    argc = 0;
    p = line;
    while (*p != '\0')
    {
        while (*p != '\0' && sh_is_space((char)*p))
        {
            p++;
        }

        if (*p == '\0')
        {
            break;
        }

        if (argc >= max_args)
        {
            return -1;
        }

        argv[argc] = sh_arg_storage[argc];
        out = 0;
        while (*p != '\0' && !sh_is_space((char)*p))
        {
            if (out + 1 < SH_LINE_MAX)
            {
                sh_arg_storage[argc][out++] = *p;
            }
            p++;
        }

        sh_arg_storage[argc][out] = '\0';
        argc++;
    }

    return argc;
}

static int sh_read_line(int8_t *line, size_t max_len)
{
    size_t len;
    size_t room;
    size_t want;
    size_t i;
    uint8_t chunk[SH_READ_CHUNK];
    uint8_t ch;
    ssize_t nread;

    /*
     * 每轮先清空输入缓冲，避免上一条命令残留在空输入或异常中断后
     * 被误当作新命令继续解析。
     */
    u_memset(line, 0, max_len);
    len = 0;
    while (1)
    {
        /*
         * 以前这里是“每个字符一次 read”，会把输入链路放大成一串 syscall
         * 和调度往返，第二个字符开始就会明显拖慢。
         *
         * 现在改成按块读取：内核侧本来就会阻塞到行结束或缓冲装满，
         * shell 一次拿回一整段，再自己做最小行编辑。
         */
        room = (max_len > len + 1) ? (max_len - len - 1) : 0;
        if (room == 0)
        {
            want = SH_READ_CHUNK;
        }
        else if (room < SH_READ_CHUNK)
        {
            want = room;
        }
        else
        {
            want = SH_READ_CHUNK;
        }

        nread = u_read(STUPIDOS_STDIN_FILENO, chunk, want);
        if (nread <= 0)
        {
            continue;
        }

        for (i = 0; i < (size_t)nread; i++)
        {
            ch = chunk[i];

            /*
             * tty 层已经在内核里接好了中断输入，这里只做最小行编辑。
             * shell 不需要知道底层是 UART 还是 virtio keyboard。
             */
            if (ch == '\r' || ch == '\n')
            {
                sh_puts((const int8_t *)"\r\n");
                line[len] = '\0';
                return (int)len;
            }

            if (ch == '\b' || ch == 0x7f)
            {
                if (len > 0)
                {
                    len--;
                    sh_puts((const int8_t *)"\b \b");
                }
                continue;
            }

            if (ch < 0x20 || ch > 0x7e)
            {
                continue;
            }

            if (len + 1 >= max_len)
            {
                continue;
            }

            line[len++] = (int8_t)ch;
            sh_putc((char)ch);
        }
    }
}

static void sh_help(void)
{
    sh_puts((const int8_t *)"commands:\r\n");
    sh_puts((const int8_t *)"  help            - show this message\r\n");
    sh_puts((const int8_t *)"  run <path> ...  - run an ELF program\r\n");
    sh_puts((const int8_t *)"  ls [path]       - execute /bin/ls\r\n");
    sh_puts((const int8_t *)"  cat <path>      - execute /bin/cat\r\n");
    sh_puts((const int8_t *)"  ping [args]     - execute /bin/ping\r\n");
    sh_puts((const int8_t *)"  sleep [args]    - execute /bin/sleep\r\n");
    sh_puts((const int8_t *)"  netcfg ...      - execute /bin/netcfg\r\n");
    sh_puts((const int8_t *)"  exit            - exit shell\r\n");
}

static int sh_exec_path(const int8_t *path, int argc, int8_t *argv[])
{
    int pid;

    pid = u_exec(path, argc, (const int8_t **)argv);
    if (pid < 0)
    {
        return pid;
    }

    /*
     * 前台 shell 需要等待子进程真正退出，才能继续显示下一个 prompt。
     * 否则像 ping / ls 这类 ELF 的输出会和 shell 自身的提示混在一起，
     * 用户看起来就像“程序没输出，等 exit 才一起出来”。
     */
    (void)u_waitpid((int32_t)pid);
    return pid;
}

int main(void)
{
    int argc;
    int ret;
    size_t cmd_len;

    (void)u_write(STUPIDOS_STDOUT_FILENO,
                  (const int8_t *)"\r\nstupidos userspace shell\r\n",
                  sizeof("\r\nstupidos userspace shell\r\n") - 1);

    while (1)
    {
        (void)u_write(STUPIDOS_STDOUT_FILENO,
                      (const int8_t *)"stupidos$ ",
                      sizeof("stupidos$ ") - 1);
        argc = sh_read_line(sh_line_buf, sizeof(sh_line_buf));
        u_memset(sh_argv_buf, 0, sizeof(sh_argv_buf));
        u_memset(sh_arg_storage, 0, sizeof(sh_arg_storage));
        argc = sh_parse_line(sh_line_buf, sh_argv_buf, SH_MAX_ARGS);
        if (argc == 0)
        {
            continue;
        }
        if (argc < 0)
        {
            sh_puts((const int8_t *)"shell: too many arguments\r\n");
            continue;
        }

        if ((uint64_t)sh_argv_buf[0] < 0x1000UL)
        {
            sh_puts((const int8_t *)"shell: invalid command pointer\r\n");
            continue;
        }

        /*
         * 把第一个 token 先复制出来，后续比较和执行都用这份稳定副本。
         * 这样即使后续有中断/调度打断，也尽量避免在同一个指针上反复读。
         */
        u_memset(sh_cmd_buf, 0, sizeof(sh_cmd_buf));
        cmd_len = 0;
        while (cmd_len + 1 < sizeof(sh_cmd_buf) && sh_argv_buf[0][cmd_len] != '\0')
        {
            sh_cmd_buf[cmd_len] = sh_argv_buf[0][cmd_len];
            cmd_len++;
        }
        sh_cmd_buf[cmd_len] = '\0';

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"help") == 0)
        {
            sh_help();
            continue;
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"exit") == 0)
        {
            u_exit(0);
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"run") == 0)
        {
            if (argc < 2)
            {
                sh_puts((const int8_t *)"usage: run <path> [args...]\r\n");
                continue;
            }

            ret = sh_exec_path(sh_argv_buf[1], argc - 1, &sh_argv_buf[1]);
            if (ret < 0)
            {
                sh_puts((const int8_t *)"run: exec failed\r\n");
                continue;
            }

            continue;
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"ls") == 0)
        {
            ret = sh_exec_path((const int8_t *)"/bin/ls", argc, sh_argv_buf);
            if (ret < 0)
            {
                sh_puts((const int8_t *)"ls: exec failed\r\n");
            }
            continue;
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"cat") == 0)
        {
            ret = sh_exec_path((const int8_t *)"/bin/cat", argc, sh_argv_buf);
            if (ret < 0)
            {
                sh_puts((const int8_t *)"cat: exec failed\r\n");
            }
            continue;
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"ping") == 0 ||
            u_strcmp(sh_cmd_buf, (const int8_t *)"nettest") == 0)
        {
            ret = sh_exec_path((const int8_t *)"/bin/ping", argc, sh_argv_buf);
            if (ret < 0)
            {
                sh_puts((const int8_t *)"ping: exec failed\r\n");
            }
            continue;
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"sleep") == 0)
        {
            ret = sh_exec_path((const int8_t *)"/bin/sleep", argc, sh_argv_buf);
            if (ret < 0)
            {
                sh_puts((const int8_t *)"sleep: exec failed\r\n");
            }
            continue;
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"netcfg") == 0)
        {
            ret = sh_exec_path((const int8_t *)"/bin/netcfg", argc, sh_argv_buf);
            if (ret < 0)
            {
                sh_puts((const int8_t *)"netcfg: exec failed\r\n");
            }
            continue;
        }

        sh_puts((const int8_t *)"unknown command\r\n");
    }

    return 0;
}
