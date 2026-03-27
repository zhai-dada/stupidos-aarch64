#include "stupidos_user.h"
#include "errno.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>

#define SH_LINE_MAX 256
#define SH_MAX_ARGS  16
#define SH_READ_CHUNK 128
#define SH_CWD_MAX STUPIDOS_PATH_MAX
#define SH_PROMPT_MAX (SH_CWD_MAX + 32)
#define SH_HISTORY_MAX 16
#define SH_COMPLETION_MAX 64

/*
 * shell 运行在一个很低层的环境里，syscall / IRQ / 调度都会反复打断它。
 * 把命令行缓冲和 argv 表放到静态区，可以避免栈帧被异常嵌套时意外踩坏，
 * 这类问题在“用户态仍跑在同一地址空间”阶段尤其容易出现。
 */
static int8_t sh_line_buf[SH_LINE_MAX];
static int8_t sh_arg_storage[SH_MAX_ARGS][SH_LINE_MAX];
static int8_t *sh_argv_buf[SH_MAX_ARGS];
static int8_t sh_cmd_buf[SH_LINE_MAX];
static int8_t sh_cwd_buf[SH_CWD_MAX];
static int8_t sh_prompt_buf[SH_PROMPT_MAX];
static int8_t sh_history[SH_HISTORY_MAX][SH_LINE_MAX];
static uint32_t sh_history_count;
static struct termios sh_saved_termios;
static bool sh_saved_termios_valid;

struct sh_completion_entry
{
    int8_t text[SH_LINE_MAX];
    bool is_dir;
};

static const int8_t *const sh_builtin_commands[] =
{
    (const int8_t *)"help",
    (const int8_t *)"cd",
    (const int8_t *)"pwd",
    (const int8_t *)"history",
    (const int8_t *)"clear",
    (const int8_t *)"echo",
    (const int8_t *)"stat",
    (const int8_t *)"uname",
    (const int8_t *)"time",
    (const int8_t *)"run",
    (const int8_t *)"ls",
    (const int8_t *)"cat",
    (const int8_t *)"ping",
    (const int8_t *)"sleep",
    (const int8_t *)"netcfg",
    (const int8_t *)"exit",
    (const int8_t *)"mkdir",
    (const int8_t *)"rmdir",
    (const int8_t *)"rm",
    (const int8_t *)"mv",
    (const int8_t *)"touch",
    (const int8_t *)"wget",
    (const int8_t *)"browser",
    (const int8_t *)"ftp",
    (const int8_t *)"ftpget",
    (const int8_t *)"ftpput",
    (const int8_t *)"tcc",
    (const int8_t *)"vi",
    (const int8_t *)"vim",
    (const int8_t *)"busybox",
    (const int8_t *)"python",
    (const int8_t *)"python3",
    (const int8_t *)"mkprobe",
    (const int8_t *)"elfinfo",
};

static const int8_t *const sh_command_search_dirs[] =
{
    (const int8_t *)"/bin",
    (const int8_t *)"/usr/bin",
    (const int8_t *)"/usr/local/bin",
    (const int8_t *)"/sbin",
};

static void sh_puts(const int8_t *str)
{
    /*
     * shell 启动阶段尽量避免把一切都交给不受控的 strlen。
     * 这样即使某个字符串指针被破坏，也只是丢一段输出，不会直接把 shell 撞死。
     */
    u_putsn(str, u_strnlen(str, 4096));
}

static void sh_restore_terminal(void)
{
    if (!sh_saved_termios_valid)
    {
        return;
    }

    /*
     * 进入 shell 时我们把终端切到 raw/noecho，方便自己做按键级回显。
     * 退出前一定要恢复回去，不然后续命令或者父 shell 会继续吃 raw 模式。
     */
    (void)tcsetattr(STUPIDOS_STDIN_FILENO, TCSANOW, &sh_saved_termios);
    sh_saved_termios_valid = false;
}

static void sh_enable_interactive_mode(void)
{
    struct termios raw;

    if (tcgetattr(STUPIDOS_STDIN_FILENO, &sh_saved_termios) < 0)
    {
        sh_saved_termios_valid = false;
        return;
    }

    raw = sh_saved_termios;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON | IXOFF);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= (tcflag_t)(CS8 | CREAD);
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STUPIDOS_STDIN_FILENO, TCSANOW, &raw) == 0)
    {
        sh_saved_termios_valid = true;
        /*
         * 让 stdout/stderr 也尽量保持“看到就出”，减少命令输出的观感延迟。
         * 即使当前 stdio 层本身已经偏向直写，这里仍然保留这层约束，
         * 方便后续接入更完整 libc / BusyBox applet 时保持一致体验。
         */
        (void)setvbuf(stdout, NULL, _IONBF, 0);
        (void)setvbuf(stderr, NULL, _IONBF, 0);
        return;
    }

    sh_saved_termios_valid = false;
}

static int sh_exec_path(const int8_t *path, int argc, int8_t *argv[]);

static void sh_put_u64(uint64_t value)
{
    int8_t buf[32];
    size_t pos;

    pos = sizeof(buf);
    buf[--pos] = '\0';
    if (value == 0)
    {
        buf[--pos] = '0';
        sh_puts(&buf[pos]);
        return;
    }

    while (value > 0 && pos > 0)
    {
        buf[--pos] = (int8_t)('0' + (value % 10ULL));
        value /= 10ULL;
    }

    sh_puts(&buf[pos]);
}

static void sh_put_hex_u64(uint64_t value)
{
    int8_t buf[34];
    size_t pos;
    static const int8_t hex[] = "0123456789abcdef";

    pos = sizeof(buf);
    buf[--pos] = '\0';
    if (value == 0)
    {
        buf[--pos] = '0';
        buf[--pos] = 'x';
        sh_puts(&buf[pos]);
        return;
    }

    while (value > 0 && pos > 2)
    {
        buf[--pos] = hex[value & 0xfULL];
        value >>= 4;
    }
    buf[--pos] = 'x';
    buf[--pos] = '0';
    sh_puts(&buf[pos]);
}

static int8_t sh_mode_type_char(uint32_t mode)
{
    switch (mode & STUPIDOS_VFS_S_IFMT)
    {
    case STUPIDOS_VFS_S_IFDIR:
        return 'd';
    case STUPIDOS_VFS_S_IFCHR:
        return 'c';
    case STUPIDOS_VFS_S_IFREG:
        return '-';
    default:
        return '?';
    }
}

static void sh_mode_perm_string(uint32_t mode, int8_t out[11])
{
    out[0] = sh_mode_type_char(mode);
    out[1] = (mode & STUPIDOS_VFS_S_IRUSR) ? 'r' : '-';
    out[2] = (mode & STUPIDOS_VFS_S_IWUSR) ? 'w' : '-';
    out[3] = (mode & STUPIDOS_VFS_S_IXUSR) ? 'x' : '-';
    out[4] = (mode & STUPIDOS_VFS_S_IRGRP) ? 'r' : '-';
    out[5] = (mode & STUPIDOS_VFS_S_IWGRP) ? 'w' : '-';
    out[6] = (mode & STUPIDOS_VFS_S_IXGRP) ? 'x' : '-';
    out[7] = (mode & STUPIDOS_VFS_S_IROTH) ? 'r' : '-';
    out[8] = (mode & STUPIDOS_VFS_S_IWOTH) ? 'w' : '-';
    out[9] = (mode & STUPIDOS_VFS_S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static void sh_refresh_cwd(void)
{
    int64_t ret;

    /*
     * prompt 不需要每次都重新向内核要 cwd。
     * 这里把 cwd 作为 shell 自己的状态缓存下来：
     * - 启动时刷新一次
     * - cd 成功后刷新一次
     * - pwd 直接打印缓存值
     */
    u_memset(sh_cwd_buf, 0, sizeof(sh_cwd_buf));
    ret = u_getcwd(sh_cwd_buf, sizeof(sh_cwd_buf));
    if (ret < 0 || sh_cwd_buf[0] == '\0')
    {
        u_memset(sh_cwd_buf, 0, sizeof(sh_cwd_buf));
        u_memcpy(sh_cwd_buf, (const int8_t *)"/", 2);
    }
}

static void sh_redraw_line(const int8_t *line)
{
    /*
     * 重绘当前输入行：
     * - 回到行首
     * - 打印 prompt
     * - 打印当前缓冲
     * - 用 ANSI 清到行尾，避免旧字符残留
     */
    (void)u_write(STUPIDOS_STDOUT_FILENO, (const int8_t *)"\r", 1);
    if (sh_prompt_buf[0] != '\0')
    {
        sh_puts(sh_prompt_buf);
    }
    if (line && line[0] != '\0')
    {
        sh_puts(line);
    }
    (void)u_write(STUPIDOS_STDOUT_FILENO, (const int8_t *)"\x1b[K", 3);
}

static bool sh_is_space(char ch);

static size_t sh_line_token_start(const int8_t *line, size_t len)
{
    size_t start;

    if (!line)
    {
        return 0;
    }

    start = len;
    while (start > 0 && !sh_is_space((char)line[start - 1]))
    {
        start--;
    }

    return start;
}

static bool sh_completion_add(struct sh_completion_entry *entries, size_t *count, const int8_t *text, bool is_dir)
{
    size_t i;

    if (!entries || !count || !text || text[0] == '\0')
    {
        return false;
    }

    for (i = 0; i < *count; i++)
    {
        if (u_strcmp(entries[i].text, text) == 0)
        {
            if (is_dir)
            {
                entries[i].is_dir = true;
            }
            return true;
        }
    }

    if (*count >= SH_COMPLETION_MAX)
    {
        return false;
    }

    u_memset(entries[*count].text, 0, sizeof(entries[*count].text));
    u_memcpy(entries[*count].text, text, u_strnlen(text, sizeof(entries[*count].text) - 1));
    entries[*count].text[sizeof(entries[*count].text) - 1] = '\0';
    entries[*count].is_dir = is_dir;
    (*count)++;
    return true;
}

static size_t sh_completion_common_prefix_len(const struct sh_completion_entry *entries, size_t count)
{
    size_t i;
    size_t pos;
    size_t min_len;

    if (!entries || count == 0)
    {
        return 0;
    }

    min_len = u_strnlen(entries[0].text, sizeof(entries[0].text) - 1);
    for (i = 1; i < count; i++)
    {
        size_t cur_len;

        cur_len = u_strnlen(entries[i].text, sizeof(entries[i].text) - 1);
        if (cur_len < min_len)
        {
            min_len = cur_len;
        }
    }

    pos = 0;
    while (pos < min_len)
    {
        int8_t ch;

        ch = entries[0].text[pos];
        for (i = 1; i < count; i++)
        {
            if (entries[i].text[pos] != ch)
            {
                return pos;
            }
        }
        pos++;
    }

    return pos;
}

static void sh_completion_print_matches(const struct sh_completion_entry *entries, size_t count)
{
    size_t i;

    if (!entries || count == 0)
    {
        return;
    }

    (void)u_write(STUPIDOS_STDOUT_FILENO, (const int8_t *)"\r\n", 2);
    for (i = 0; i < count; i++)
    {
        sh_puts(entries[i].text);
        if (entries[i].is_dir)
        {
            sh_puts((const int8_t *)"/");
        }
        sh_puts((const int8_t *)"\r\n");
    }
}

static bool sh_scan_directory_matches(const int8_t *dirpath,
                                      const int8_t *prefix,
                                      size_t prefix_len,
                                      const int8_t *repl_prefix,
                                      size_t repl_prefix_len,
                                      struct sh_completion_entry *entries,
                                      size_t *count,
                                      bool files_only)
{
    struct stupidos_dirent ent;
    int8_t candidate[SH_LINE_MAX];
    size_t index;
    size_t name_len;
    int ret;
    bool found;

    if (!dirpath || !prefix || !entries || !count)
    {
        return false;
    }

    found = false;
    for (index = 0; ; index++)
    {
        ret = u_readdir(dirpath, (uint32_t)index, &ent);
        if (ret == -STUPIDOS_ENOENT)
        {
            break;
        }
        if (ret < 0)
        {
            return found;
        }

        if (ent.name[0] == '\0')
        {
            continue;
        }

        if (strncmp((const char *)ent.name, (const char *)prefix, prefix_len) != 0)
        {
            continue;
        }

        if (files_only && ((ent.mode & STUPIDOS_VFS_S_IFMT) == STUPIDOS_VFS_S_IFDIR))
        {
            continue;
        }

        u_memset(candidate, 0, sizeof(candidate));
        if (repl_prefix && repl_prefix_len > 0)
        {
            u_memcpy(candidate, repl_prefix, repl_prefix_len);
        }

        name_len = u_strnlen((const int8_t *)ent.name, sizeof(candidate) - repl_prefix_len - 1);
        if (repl_prefix_len + name_len >= sizeof(candidate))
        {
            continue;
        }

        u_memcpy(candidate + repl_prefix_len, ent.name, name_len);
        candidate[repl_prefix_len + name_len] = '\0';

        if (!sh_completion_add(entries, count, candidate, ((ent.mode & STUPIDOS_VFS_S_IFMT) == STUPIDOS_VFS_S_IFDIR)))
        {
            continue;
        }

        found = true;
    }

    return found;
}

static int sh_complete_line(int8_t *line, size_t max_len, size_t *len_io)
{
    struct sh_completion_entry entries[SH_COMPLETION_MAX];
    int8_t dirbuf[STUPIDOS_PATH_MAX];
    size_t token_start;
    size_t token_len;
    size_t i;
    size_t count;
    size_t common_len;
    const int8_t *token;
    const char *slash;
    const char *name_prefix;
    size_t name_prefix_len;
    size_t repl_prefix_len;
    size_t dir_len;
    bool command_mode;
    bool any;

    if (!line || !len_io)
    {
        return -1;
    }

    token_start = sh_line_token_start(line, *len_io);
    token_len = *len_io - token_start;
    token = &line[token_start];
    slash = strchr((const char *)token, '/');
    command_mode = (token_start == 0 && !slash);
    count = 0;
    any = false;
    u_memset(entries, 0, sizeof(entries));
    u_memset(dirbuf, 0, sizeof(dirbuf));

    if (command_mode)
    {
        for (i = 0; i < sizeof(sh_builtin_commands) / sizeof(sh_builtin_commands[0]); i++)
        {
            if (strncmp((const char *)sh_builtin_commands[i], (const char *)token, token_len) == 0)
            {
                if (sh_completion_add(entries, &count, sh_builtin_commands[i], false))
                {
                    any = true;
                }
            }
        }

        for (i = 0; i < sizeof(sh_command_search_dirs) / sizeof(sh_command_search_dirs[0]); i++)
        {
            if (sh_scan_directory_matches(sh_command_search_dirs[i], token, token_len, 0, 0, entries, &count, true))
            {
                any = true;
            }
        }
    }
    else
    {
        if (slash)
        {
            dir_len = (size_t)((const int8_t *)slash - token) + 1U;
            if (dir_len >= sizeof(dirbuf))
            {
                dir_len = sizeof(dirbuf) - 1U;
            }
            u_memcpy(dirbuf, token, dir_len);
            dirbuf[dir_len] = '\0';
            name_prefix = slash + 1;
            name_prefix_len = token_len - dir_len;
            repl_prefix_len = dir_len;
        }
        else
        {
            if (u_getcwd(dirbuf, sizeof(dirbuf)) < 0 || dirbuf[0] == '\0')
            {
                u_memcpy(dirbuf, (const int8_t *)".", 2);
            }
            name_prefix = (const char *)token;
            name_prefix_len = token_len;
            repl_prefix_len = 0;
        }

        any = sh_scan_directory_matches(dirbuf,
                                        (const int8_t *)name_prefix,
                                        name_prefix_len,
                                        token,
                                        repl_prefix_len,
                                        entries,
                                        &count,
                                        false);
    }

    if (!any || count == 0)
    {
        return 0;
    }

    common_len = sh_completion_common_prefix_len(entries, count);
    if (count == 1 || common_len > token_len)
    {
        size_t add_len;
        size_t base_len;
        const int8_t *extra;
        size_t target_len;

        base_len = token_start;
        target_len = (count == 1) ? u_strnlen(entries[0].text, sizeof(entries[0].text) - 1) : common_len;
        if (target_len < token_len)
        {
            return 0;
        }
        extra = &entries[0].text[token_len];
        add_len = target_len - token_len;
        if (base_len + target_len + 2U >= max_len)
        {
            add_len = (max_len > base_len + token_len + 1U) ? (max_len - base_len - token_len - 1U) : 0U;
        }

        if (add_len > 0)
        {
            u_memcpy(&line[base_len + token_len], extra, add_len);
            token_len += add_len;
            line[base_len + token_len] = '\0';
            *len_io = base_len + token_len;
        }

        if (count == 1)
        {
            if (command_mode)
            {
            if (*len_io + 1 < max_len)
            {
                line[*len_io] = ' ';
                (*len_io)++;
                line[*len_io] = '\0';
            }
        }
        else if (entries[0].is_dir)
        {
            if (*len_io + 1 < max_len)
                {
                    line[*len_io] = '/';
                    (*len_io)++;
                    line[*len_io] = '\0';
                }
            }
        }

        sh_redraw_line(line);
        return 1;
    }

    sh_completion_print_matches(entries, count);
    sh_redraw_line(line);
    return 1;
}

static int32_t sh_history_oldest_index(void)
{
    if (sh_history_count <= SH_HISTORY_MAX)
    {
        return 0;
    }

    return (int32_t)(sh_history_count - SH_HISTORY_MAX);
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
    uint8_t echo_chunk[SH_READ_CHUNK * 4];
    uint8_t ch;
    ssize_t nread;
    int32_t history_pos;
    int32_t history_oldest;
    size_t saved_len;
    int8_t saved_line[SH_LINE_MAX];
    uint8_t esc_state;
    bool esc_seq_active;
    size_t echo_len_total;

    echo_len_total = 0;

    /*
     * 每轮先清空输入缓冲，避免上一条命令残留在空输入或异常中断后
     * 被误当作新命令继续解析。
     */
    u_memset(line, 0, max_len);
    u_memset(saved_line, 0, sizeof(saved_line));
    len = 0;
    saved_len = 0;
    history_pos = -1;
    history_oldest = 0;
    esc_state = 0;
    esc_seq_active = false;
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

            if (esc_state != 0)
            {
                if (esc_state == 1)
                {
                    if (ch == '[')
                    {
                        esc_state = 2;
                        continue;
                    }

                    /*
                     * 关键修复（中文）：
                     * 之前非 CSI 的 ESC 序列会无条件吞掉“下一个字节”，
                     * 导致回车偶发被吃掉，表象就是“按 Enter 没反应”。
                     *
                     * 现在改成：
                     * - 可打印字符按“非标准转义片段”丢弃；
                     * - 控制字符（如 \\r/\\n/退格）继续走正常处理流程。
                     */
                    esc_state = 0;
                    if (ch >= 0x20 && ch <= 0x7e)
                    {
                        continue;
                    }
                }

                if (esc_state == 2)
                {
                    /*
                     * CSI 参数字节（0x20..0x3f）继续消费，直到终止字节。
                     */
                    if (ch >= 0x20 && ch <= 0x3f)
                    {
                        continue;
                    }

                    if (ch >= 0x40 && ch <= 0x7e)
                    {
                        esc_state = 0;
                        if (ch == 'A' || ch == 'B')
                        {
                            uint32_t history_count;
                            const int8_t *src;

                            history_count = sh_history_count;
                            if (history_count == 0)
                            {
                                continue;
                            }

                            if (!esc_seq_active)
                            {
                                u_memset(saved_line, 0, sizeof(saved_line));
                                u_memcpy(saved_line, line, len);
                                saved_len = len;
                                esc_seq_active = true;
                                history_pos = (int32_t)history_count - 1;
                                history_oldest = sh_history_oldest_index();
                            }

                            if (ch == 'A')
                            {
                                if (history_pos > history_oldest)
                                {
                                    history_pos--;
                                }
                            }
                            else if (ch == 'B')
                            {
                                if (history_pos < (int32_t)history_count - 1)
                                {
                                    history_pos++;
                                }
                                else
                                {
                                    history_pos = -1;
                                }
                            }

                            u_memset(line, 0, max_len);
                            if (history_pos >= 0)
                            {
                                src = sh_history[(uint32_t)history_pos % SH_HISTORY_MAX];
                                u_memcpy(line, src, u_strnlen(src, max_len - 1));
                                len = u_strnlen(src, max_len - 1);
                            }
                            else
                            {
                                u_memcpy(line, saved_line, saved_len);
                                len = saved_len;
                            }

                            line[len] = '\0';
                            sh_redraw_line(line);
                        }
                        continue;
                    }

                    /*
                     * 异常序列：尽快退出 ESC 状态，避免后续普通输入被长期吞掉。
                     */
                    esc_state = 0;
                }
            }

            if (ch == 0x1b)
            {
                esc_state = 1;
                continue;
            }

            if (ch == '\t')
            {
                size_t new_len;

                esc_state = 0;
                esc_seq_active = false;
                new_len = len;
                if (sh_complete_line(line, max_len, &new_len) > 0)
                {
                    len = new_len;
                }
                continue;
            }

            /*
             * tty 层已经在内核里接好了中断输入，这里只做最小行编辑。
             * shell 不需要知道底层是 UART 还是 virtio keyboard。
             */
            if (ch == '\r' || ch == '\n')
            {
                /*
                 * 终端输入的最后一击也要立刻回显。
                 * 之前很多环境里内核并不会替我们做好行回显，
                 * 所以这里主动补一行换行，确保“按回车就有反馈”。
                 */
                if (echo_len_total + 2 <= sizeof(echo_chunk))
                {
                    echo_chunk[echo_len_total++] = '\r';
                    echo_chunk[echo_len_total++] = '\n';
                }
                if (echo_len_total > 0)
                {
                    (void)u_write(STUPIDOS_STDOUT_FILENO, (const int8_t *)echo_chunk, echo_len_total);
                    echo_len_total = 0;
                }
                line[len] = '\0';
                esc_seq_active = false;
                return (int)len;
            }

            if (ch == '\b' || ch == 0x7f)
            {
                if (len > 0)
                {
                    len--;
                    line[len] = '\0';
                    /*
                     * 本地回显退格：擦掉上一字符，保持 shell 的输入反馈即时。
                     * 这里也进入批量缓冲，避免每个字符都立即做一次系统调用。
                     */
                    if (echo_len_total + 3 <= sizeof(echo_chunk))
                    {
                        echo_chunk[echo_len_total++] = '\b';
                        echo_chunk[echo_len_total++] = ' ';
                        echo_chunk[echo_len_total++] = '\b';
                    }
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
            line[len] = '\0';
            /*
             * 主动做一次本地回显，避免依赖内核/TTY 的 echo 行为。
             * 这样即使底层 tty 没有打开 ECHO，用户也能立刻看到输入。
             */
            if (echo_len_total + 1 <= sizeof(echo_chunk))
            {
                echo_chunk[echo_len_total++] = ch;
            }
        }

        if (echo_len_total > 0)
        {
            (void)u_write(STUPIDOS_STDOUT_FILENO, (const int8_t *)echo_chunk, echo_len_total);
            echo_len_total = 0;
        }
    }
}

static void sh_help(void)
{
    sh_puts((const int8_t *)"commands:\r\n");
    sh_puts((const int8_t *)"  help            - show this message\r\n");
    sh_puts((const int8_t *)"  cd <path>       - change current directory\r\n");
    sh_puts((const int8_t *)"  pwd             - print current directory\r\n");
    sh_puts((const int8_t *)"  history         - show recent commands\r\n");
    sh_puts((const int8_t *)"  !!              - repeat last command\r\n");
    sh_puts((const int8_t *)"  clear           - clear the terminal screen\r\n");
    sh_puts((const int8_t *)"  echo [args...]  - execute /bin/echo\r\n");
    sh_puts((const int8_t *)"  stat [path]     - show file metadata\r\n");
    sh_puts((const int8_t *)"  uname           - show system identity\r\n");
    sh_puts((const int8_t *)"  time            - show current time\r\n");
    sh_puts((const int8_t *)"  run <path> ...  - run an ELF program\r\n");
    sh_puts((const int8_t *)"  ls [path]       - execute /bin/ls\r\n");
    sh_puts((const int8_t *)"  cat <path>      - execute /bin/cat\r\n");
    sh_puts((const int8_t *)"  ping [args]     - execute /bin/ping\r\n");
    sh_puts((const int8_t *)"  sleep [args]    - execute /bin/sleep\r\n");
    sh_puts((const int8_t *)"  netcfg ...      - execute /bin/netcfg\r\n");
    sh_puts((const int8_t *)"  browser <url>   - open a web page in terminal UI\r\n");
    sh_puts((const int8_t *)"  exit            - exit shell\r\n");
}

static size_t sh_prompt(void)
{
    size_t len;

    u_memset(sh_prompt_buf, 0, sizeof(sh_prompt_buf));
    if (sh_cwd_buf[0] == '\0')
    {
        sh_refresh_cwd();
    }

    len = 0;
    u_memcpy(&sh_prompt_buf[len], (const int8_t *)"stupidos:", sizeof("stupidos:") - 1);
    len += sizeof("stupidos:") - 1;
    u_memcpy(&sh_prompt_buf[len], sh_cwd_buf, u_strnlen(sh_cwd_buf, sizeof(sh_cwd_buf) - 1));
    len += u_strnlen(sh_cwd_buf, sizeof(sh_cwd_buf) - 1);
    u_memcpy(&sh_prompt_buf[len], (const int8_t *)"$ ", sizeof("$ ") - 1);
    len += sizeof("$ ") - 1;
    sh_prompt_buf[len] = '\0';
    (void)u_write(STUPIDOS_STDOUT_FILENO, sh_prompt_buf, len);
    return len;
}

static void sh_history_add(const int8_t *line)
{
    uint32_t slot;

    if (!line || line[0] == '\0')
    {
        return;
    }

    slot = sh_history_count % SH_HISTORY_MAX;
    u_memset(sh_history[slot], 0, sizeof(sh_history[slot]));
    u_memcpy(sh_history[slot], line, u_strnlen(line, SH_LINE_MAX - 1));
    sh_history[slot][SH_LINE_MAX - 1] = '\0';
    sh_history_count++;
}

static int sh_history_expand(int8_t *line, size_t line_len)
{
    const int8_t *src;
    size_t len;
    uint32_t index;

    if (!line || line_len == 0)
    {
        return -1;
    }

    if (line[0] != '!')
    {
        return 0;
    }

    if (line[1] == '\0')
    {
        if (sh_history_count == 0)
        {
            return -1;
        }

        src = sh_history[(sh_history_count - 1) % SH_HISTORY_MAX];
    }
    else if (line[1] == '!' && line[2] == '\0')
    {
        if (sh_history_count == 0)
        {
            return -1;
        }

        src = sh_history[(sh_history_count - 1) % SH_HISTORY_MAX];
    }
    else
    {
        index = 0;
        for (len = 1; line[len] != '\0'; len++)
        {
            if (line[len] < '0' || line[len] > '9')
            {
                return -1;
            }

            index = index * 10U + (uint32_t)(line[len] - '0');
        }

        if (index == 0 || index > sh_history_count)
        {
            return -1;
        }

        src = sh_history[(index - 1) % SH_HISTORY_MAX];
    }

    u_memset(line, 0, line_len);
    u_memcpy(line, src, u_strnlen(src, line_len - 1));
    line[line_len - 1] = '\0';
    return 1;
}

static void sh_history_print(void)
{
    uint32_t start;
    uint32_t i;
    uint32_t count;

    count = (sh_history_count < SH_HISTORY_MAX) ? sh_history_count : SH_HISTORY_MAX;
    if (count == 0)
    {
        sh_puts((const int8_t *)"history: empty\r\n");
        return;
    }

    start = (sh_history_count > SH_HISTORY_MAX) ? (sh_history_count - SH_HISTORY_MAX) : 0;
    for (i = 0; i < count; i++)
    {
        uint32_t slot;

        slot = (start + i) % SH_HISTORY_MAX;
        sh_puts((const int8_t *)"  ");
        {
            int8_t num_buf[16];
            int8_t *p;
            uint32_t n;

            p = &num_buf[sizeof(num_buf) - 1];
            *p = '\0';
            n = start + i + 1;
            if (n == 0)
            {
                *--p = '0';
            }
            else
            {
                while (n > 0 && p > num_buf)
                {
                    *--p = (int8_t)('0' + (n % 10U));
                    n /= 10U;
                }
            }
            sh_puts(p);
        }
        sh_puts((const int8_t *)"  ");
        sh_puts(sh_history[slot]);
        sh_puts((const int8_t *)"\r\n");
    }
}

static void sh_clear_screen(void)
{
    /*
     * 清屏后回到左上角。
     * shell 下一轮会重新打印 prompt，所以这里只做最小的终端清理。
     */
    (void)u_write(STUPIDOS_STDOUT_FILENO, (const int8_t *)"\x1b[2J\x1b[H", 7);
}

static void sh_stat_cmd(const int8_t *path)
{
    struct stupidos_stat st;
    int8_t perm[11];
    int ret;

    ret = u_stat(path, &st);
    if (ret < 0)
    {
        sh_puts((const int8_t *)"stat: failed\r\n");
        return;
    }

    sh_puts((const int8_t *)"\r\n");
    sh_puts((const int8_t *)"  File: ");
    sh_puts(path);
    sh_puts((const int8_t *)"\r\n");
    sh_mode_perm_string((uint32_t)st.mode, perm);
    sh_puts((const int8_t *)"  Mode: ");
    sh_puts(perm);
    sh_puts((const int8_t *)" (");
    sh_put_hex_u64((uint64_t)st.mode);
    sh_puts((const int8_t *)")\r\n");
    sh_puts((const int8_t *)"  Links: ");
    sh_put_u64((uint64_t)st.nlink);
    sh_puts((const int8_t *)"  UID: ");
    sh_put_u64((uint64_t)st.uid);
    sh_puts((const int8_t *)"  GID: ");
    sh_put_u64((uint64_t)st.gid);
    sh_puts((const int8_t *)"\r\n");
    sh_puts((const int8_t *)"  Size: ");
    sh_put_u64(st.size);
    sh_puts((const int8_t *)"  Blocks: ");
    sh_put_u64(st.blocks);
    sh_puts((const int8_t *)"  Blksize: ");
    sh_put_u64((uint64_t)st.blksize);
    sh_puts((const int8_t *)"\r\n  Ino: ");
    sh_put_u64((uint64_t)st.ino);
    sh_puts((const int8_t *)"\r\n");
}

static void sh_uname_cmd(void)
{
    struct stupidos_utsname uts;
    int ret;

    ret = u_uname(&uts);
    if (ret < 0)
    {
        sh_puts((const int8_t *)"uname: failed\r\n");
        return;
    }

    sh_puts((const int8_t *)"\r\n");
    sh_puts(uts.sysname);
    sh_puts((const int8_t *)" ");
    sh_puts(uts.nodename);
    sh_puts((const int8_t *)" ");
    sh_puts(uts.release);
    sh_puts((const int8_t *)" ");
    sh_puts(uts.version);
    sh_puts((const int8_t *)" ");
    sh_puts(uts.machine);
    sh_puts((const int8_t *)"\r\n");
}

static void sh_time_cmd(void)
{
    struct stupidos_timeval tv;
    int64_t ret;

    ret = u_gettimeofday(&tv);
    if (ret < 0)
    {
        sh_puts((const int8_t *)"time: failed\r\n");
        return;
    }

    sh_puts((const int8_t *)"\r\ntime=");
    sh_put_u64((uint64_t)tv.tv_sec);
    sh_puts((const int8_t *)".");
    if (tv.tv_usec < 100000)
    {
        sh_puts((const int8_t *)"0");
        if (tv.tv_usec < 10000)
        {
            sh_puts((const int8_t *)"0");
            if (tv.tv_usec < 1000)
            {
                sh_puts((const int8_t *)"0");
                if (tv.tv_usec < 100)
                {
                    sh_puts((const int8_t *)"0");
                    if (tv.tv_usec < 10)
                    {
                        sh_puts((const int8_t *)"0");
                    }
                }
            }
        }
    }
    sh_put_u64((uint64_t)tv.tv_usec);
    sh_puts((const int8_t *)" s\r\n");
}

static bool sh_contains_slash(const int8_t *path)
{
    size_t i;

    if (!path)
    {
        return false;
    }

    for (i = 0; path[i] != '\0'; i++)
    {
        if (path[i] == '/')
        {
            return true;
        }
    }

    return false;
}

static int sh_exec_search(int argc, int8_t *argv[])
{
    /*
     * 这里不能用“字符串指针数组”保存前缀。
     *
     * 我们当前的 userspace ELF 加载器只负责把段复制到目标地址，
     * 不做动态重定位。若把 `const char *prefixes[]` 直接编进 rodata，
     * 里面的指针值会保留为链接期地址（例如 0x24e0 这种低地址）。
     * 一旦 shell 运行到这里，就会把这些旧地址当真实指针使用并触发异常。
     *
     * 改成二维字符数组后，每个前缀都跟随本体存放，不再依赖重定位。
     */
    static const int8_t prefixes[][16] =
    {
        "",
        "/bin/",
        "/usr/bin/",
        "/sbin/",
    };
    int8_t path[STUPIDOS_PATH_MAX];
    size_t arg_len;
    size_t prefix_len;
    size_t i;
    int ret;

    if (argc <= 0 || !argv || !argv[0] || argv[0][0] == '\0')
    {
        return -EINVAL;
    }

    if (sh_contains_slash(argv[0]))
    {
        return sh_exec_path(argv[0], argc, argv);
    }

    arg_len = u_strnlen(argv[0], sizeof(path) - 1);
    for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
    {
        prefix_len = u_strnlen(prefixes[i], sizeof(prefixes[i]) - 1);
        if (prefix_len + arg_len + 1 > sizeof(path))
        {
            continue;
        }

        u_memset(path, 0, sizeof(path));
        if (prefix_len)
        {
            u_memcpy(path, prefixes[i], prefix_len);
        }
        u_memcpy(path + prefix_len, argv[0], arg_len);
        path[prefix_len + arg_len] = '\0';

        ret = sh_exec_path(path, argc, argv);
        if (ret >= 0)
        {
            return ret;
        }

        if (ret != -ENOENT)
        {
            return ret;
        }
    }

    /*
     * BusyBox 兼容回退：
     * 如果独立 ELF 不存在，就尝试让 /bin/busybox 代劳。
     * 这能让后续新增 applet 时，shell 直接获得“busybox applet”体验，
     * 不需要每个命令都单独落一个二进制。
     */
    if (u_strcmp(argv[0], (const int8_t *)"busybox") == 0)
    {
        return -ENOENT;
    }
    if (argc + 1 < SH_MAX_ARGS + 2)
    {
        int8_t *busybox_argv[SH_MAX_ARGS + 2];

        busybox_argv[0] = (int8_t *)"busybox";
        busybox_argv[1] = argv[0];
        for (i = 1; i < argc && i + 1 < (int)(sizeof(busybox_argv) / sizeof(busybox_argv[0])); i++)
        {
            busybox_argv[i + 1] = argv[i];
        }
        busybox_argv[argc + 1] = NULL;
        ret = sh_exec_path((const int8_t *)"/bin/busybox", argc + 1, busybox_argv);
        if (ret >= 0 || ret != -ENOENT)
        {
            return ret;
        }
    }

    return -ENOENT;
}

static int sh_exec_path(const int8_t *path, int argc, int8_t *argv[])
{
    int pid;

    sh_restore_terminal();
    (void)fflush(NULL);
    pid = u_exec(path, argc, (const int8_t **)argv);
    if (pid < 0)
    {
        sh_enable_interactive_mode();
        return pid;
    }

    /*
     * 前台 shell 需要等待子进程真正退出，才能继续显示下一个 prompt。
     * 否则像 ping / ls 这类 ELF 的输出会和 shell 自身的提示混在一起，
     * 用户看起来就像“程序没输出，等 exit 才一起出来”。
     */
    (void)u_waitpid((int32_t)pid);
    sh_enable_interactive_mode();
    return pid;
}

int main(void)
{
    int argc;
    int ret;
    size_t cmd_len;
    size_t prompt_len;

    (void)atexit(sh_restore_terminal);

    (void)u_write(STUPIDOS_STDOUT_FILENO,
                  (const int8_t *)"\r\nstupidos userspace shell\r\n",
                  sizeof("\r\nstupidos userspace shell\r\n") - 1);

    /*
     * 把终端切到 shell 自己接管输入的模式（中文）：
     * - 禁用 canonical，按键立即送到 shell；
     * - 禁用 echo，由 shell 自己做即时回显；
     * - 这样输入响应不会卡在“等整行提交”的 tty 层。
     */
    sh_enable_interactive_mode();

    /*
     * 默认工作目录切到可写的 /tmp（中文）：
     * 当前根文件系统仍以只读能力为主，用户在 "/" 下直接 mkdir 会看到 EROFS。
     * 这里先把默认 cwd 放到 /tmp，保证开箱即用的交互体验更接近 Linux 习惯。
     */
    if (u_chdir((const int8_t *)"/tmp") < 0)
    {
        (void)u_chdir((const int8_t *)"/");
    }
    sh_refresh_cwd();

    while (1)
    {
        prompt_len = sh_prompt();
        (void)prompt_len;
        argc = sh_read_line(sh_line_buf, sizeof(sh_line_buf));
        ret = sh_history_expand(sh_line_buf, sizeof(sh_line_buf));
        if (ret < 0)
        {
            sh_puts((const int8_t *)"history: no such entry\r\n");
            continue;
        }
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
            sh_history_add(sh_line_buf);
            continue;
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"pwd") == 0)
        {
            sh_refresh_cwd();
            sh_puts(sh_cwd_buf);
            sh_puts((const int8_t *)"\r\n");
            sh_history_add(sh_line_buf);
            continue;
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"cd") == 0)
        {
            const int8_t *target;

            target = (argc >= 2 && sh_argv_buf[1]) ? (const int8_t *)sh_argv_buf[1] : (const int8_t *)"/";
            ret = (int)u_chdir(target);
            if (ret < 0)
            {
                sh_puts((const int8_t *)"cd: failed\r\n");
            }
            else
            {
                sh_refresh_cwd();
            }
            sh_history_add(sh_line_buf);
            continue;
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"exit") == 0)
        {
            sh_history_add(sh_line_buf);
            sh_restore_terminal();
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

            sh_history_add(sh_line_buf);
            continue;
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"history") == 0)
        {
            sh_history_print();
            sh_history_add(sh_line_buf);
            continue;
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"clear") == 0)
        {
            sh_clear_screen();
            sh_history_add(sh_line_buf);
            continue;
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"stat") == 0)
        {
            const int8_t *target;

            target = (argc >= 2 && sh_argv_buf[1]) ? (const int8_t *)sh_argv_buf[1] : sh_cwd_buf;
            sh_stat_cmd(target);
            sh_history_add(sh_line_buf);
            continue;
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"uname") == 0)
        {
            sh_uname_cmd();
            sh_history_add(sh_line_buf);
            continue;
        }

        if (u_strcmp(sh_cmd_buf, (const int8_t *)"time") == 0)
        {
            sh_time_cmd();
            sh_history_add(sh_line_buf);
            continue;
        }

        ret = sh_exec_search(argc, sh_argv_buf);
        if (ret >= 0)
        {
            sh_history_add(sh_line_buf);
            continue;
        }

        sh_puts((const int8_t *)"unknown command\r\n");
        sh_history_add(sh_line_buf);
    }

    return 0;
}
