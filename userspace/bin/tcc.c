#include "stupidos_user.h"
#include "errno.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * tcc 迁移阶段程序（stage-2）。
 *
 * 说明：
 * 1. 用户希望在系统内直接运行 tinycc；
 * 2. 当前先把命令入口和常见参数稳定下来，方便脚本先接入；
 * 3. 后续再把 tinycc 前端/后端真实接进来，替换这里的占位逻辑。
 */

static void tcc_help(void)
{
    u_puts((const int8_t *)"tcc: tinycc bootstrap frontend (stupidos)\n");
    u_puts((const int8_t *)"usage: tcc [options] file...\n");
    u_puts((const int8_t *)"  -h, --help          show this help\n");
    u_puts((const int8_t *)"  -v, --version       show bootstrap version\n");
    u_puts((const int8_t *)"  --syscall-check     check libc/syscall baseline\n");
}

static bool tcc_opt_takes_value(const char *arg)
{
    if (!arg)
    {
        return false;
    }

    return u_strcmp((const int8_t *)arg, (const int8_t *)"-o") == 0 ||
           u_strcmp((const int8_t *)arg, (const int8_t *)"-I") == 0 ||
           u_strcmp((const int8_t *)arg, (const int8_t *)"-L") == 0 ||
           u_strcmp((const int8_t *)arg, (const int8_t *)"-B") == 0 ||
           u_strcmp((const int8_t *)arg, (const int8_t *)"-isystem") == 0 ||
           u_strcmp((const int8_t *)arg, (const int8_t *)"-include") == 0 ||
           u_strcmp((const int8_t *)arg, (const int8_t *)"-MF") == 0 ||
           u_strcmp((const int8_t *)arg, (const int8_t *)"-MT") == 0 ||
           u_strcmp((const int8_t *)arg, (const int8_t *)"-MQ") == 0;
}

static bool tcc_is_no_link_opt(const char *arg)
{
    if (!arg)
    {
        return false;
    }

    return u_strcmp((const int8_t *)arg, (const int8_t *)"-c") == 0 ||
           u_strcmp((const int8_t *)arg, (const int8_t *)"-E") == 0 ||
           u_strcmp((const int8_t *)arg, (const int8_t *)"-S") == 0 ||
           u_strcmp((const int8_t *)arg, (const int8_t *)"-shared") == 0 ||
           u_strcmp((const int8_t *)arg, (const int8_t *)"-run") == 0;
}

static bool tcc_needs_link_stage(int argc, char **argv)
{
    bool end_of_options;
    bool skip_next;
    bool has_input;
    int i;

    end_of_options = false;
    skip_next = false;
    has_input = false;

    for (i = 1; i < argc; i++)
    {
        const char *arg;

        arg = argv[i];
        if (!arg)
        {
            continue;
        }

        if (skip_next)
        {
            skip_next = false;
            continue;
        }

        if (!end_of_options)
        {
            if (u_strcmp((const int8_t *)arg, (const int8_t *)"--") == 0)
            {
                end_of_options = true;
                continue;
            }

            if (tcc_is_no_link_opt(arg))
            {
                return false;
            }

            if (tcc_opt_takes_value(arg))
            {
                skip_next = true;
                continue;
            }

            if (arg[0] == '-' && arg[1] != '\0')
            {
                continue;
            }
        }

        has_input = true;
    }

    return has_input;
}

static int tcc_try_exec_real(int argc, char **argv)
{
    const int8_t *exec_argv[192];
    bool link_mode;
    bool has_runtime_libs;
    bool has_libgcc;
    int outc;
    int i;
    int64_t pid;
    int64_t waited;

    if (argc <= 0)
    {
        return -E2BIG;
    }

    link_mode = tcc_needs_link_stage(argc, argv);
    has_runtime_libs = (access("/usr/lib/crt0.o", F_OK) == 0 &&
                        access("/usr/lib/libstupidos.a", F_OK) == 0);
    has_libgcc = (access("/usr/lib/libgcc.a", F_OK) == 0);
    outc = 0;

    /*
     * 关键兼容层：
     * 1) 固定传入 -B /usr/local/lib/tcc，确保 tccdefs.h 与私有头可见；
     * 2) 链接阶段自动追加 stupidos 运行时对象，做到 guest 内“开箱即编译可执行文件”。
     */
    exec_argv[outc++] = (argc > 0 && argv[0]) ? (const int8_t *)argv[0] : (const int8_t *)"tcc";
    exec_argv[outc++] = (const int8_t *)"-B";
    exec_argv[outc++] = (const int8_t *)"/usr/local/lib/tcc";
    exec_argv[outc++] = (const int8_t *)"-isystem";
    exec_argv[outc++] = (const int8_t *)"/usr/include";
    for (i = 1; i < argc; i++)
    {
        if (!argv[i])
        {
            continue;
        }
        if (outc >= (int)(sizeof(exec_argv) / sizeof(exec_argv[0])))
        {
            return -E2BIG;
        }
        exec_argv[outc++] = (const int8_t *)argv[i];
    }

    if (link_mode && has_runtime_libs)
    {
        if (outc + 4 >= (int)(sizeof(exec_argv) / sizeof(exec_argv[0])))
        {
            return -E2BIG;
        }
        exec_argv[outc++] = (const int8_t *)"-nostdlib";
        exec_argv[outc++] = (const int8_t *)"/usr/lib/crt0.o";
        exec_argv[outc++] = (const int8_t *)"/usr/lib/libstupidos.a";
        if (has_libgcc)
        {
            exec_argv[outc++] = (const int8_t *)"/usr/lib/libgcc.a";
        }
    }

    pid = u_exec((const int8_t *)"/bin/tcc.real", outc, exec_argv);
    if (pid < 0)
    {
        return (int)pid;
    }

    waited = u_waitpid((int32_t)pid);
    if (waited < 0)
    {
        return (int)waited;
    }
    return 0;
}

static int tcc_syscall_check(void)
{
    int ret;
    char *cwd;
    const char *check_dir;

    u_puts((const int8_t *)"tcc: checking libc/syscall baseline...\n");
    check_dir = "/tmp/tcc-check";

    /*
     * /tmp 现在是可写 ramfs，检查逻辑改成“可创建 + 可删除”。
     * 如果上次异常退出残留了目录，这里先尝试清理一次并重试。
     */
    ret = mkdir(check_dir, 0755);
    if (ret < 0 && errno == EEXIST)
    {
        (void)rmdir(check_dir);
        ret = mkdir(check_dir, 0755);
    }

    if (ret == 0)
    {
        u_puts((const int8_t *)"  mkdir: ok\n");
    }
    else
    {
        u_puts((const int8_t *)"  mkdir: failed\n");
        return 1;
    }

    ret = rmdir(check_dir);
    if (ret == 0)
    {
        u_puts((const int8_t *)"  rmdir: ok\n");
    }
    else
    {
        u_puts((const int8_t *)"  rmdir: failed\n");
        return 1;
    }

    cwd = getcwd(0, 0);
    if (cwd)
    {
        u_puts((const int8_t *)"  getcwd(NULL,0): ok\n");
        free(cwd);
    }
    else
    {
        u_puts((const int8_t *)"  getcwd(NULL,0): failed\n");
        return 1;
    }

    u_puts((const int8_t *)"tcc: baseline check passed\n");
    return 0;
}

int main(int argc, char **argv)
{
    int i;
    int ret;
    bool has_stage2_only_arg;
    bool has_real_work_arg;

    if (argc <= 1)
    {
        tcc_help();
        return 0;
    }

    has_stage2_only_arg = false;
    has_real_work_arg = false;
    for (i = 1; i < argc; i++)
    {
        if (!argv[i])
        {
            continue;
        }

        if (u_strcmp((const int8_t *)argv[i], (const int8_t *)"-h") == 0 ||
            u_strcmp((const int8_t *)argv[i], (const int8_t *)"--help") == 0)
        {
            tcc_help();
            return 0;
        }

        if (u_strcmp((const int8_t *)argv[i], (const int8_t *)"--syscall-check") == 0)
        {
            return tcc_syscall_check();
        }

        if (u_strcmp((const int8_t *)argv[i], (const int8_t *)"--stage2-only") == 0)
        {
            has_stage2_only_arg = true;
            continue;
        }
        has_real_work_arg = true;
    }

    if (has_real_work_arg && !has_stage2_only_arg)
    {
        ret = tcc_try_exec_real(argc, argv);
        if (ret == 0)
        {
            return 0;
        }
        u_puts((const int8_t *)"tcc: /bin/tcc.real unavailable, fallback to stage2 mode\n");
    }

    if (!has_stage2_only_arg)
    {
        u_puts((const int8_t *)"tcc: compile pipeline is not linked yet\n");
        u_puts((const int8_t *)"tcc: next step is integrating tinycc parser/codegen into /bin/tcc\n");
        return 2;
    }

    return 0;
}
