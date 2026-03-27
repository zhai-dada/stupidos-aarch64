#include "stupidos_user.h"
#include "errno.h"
#include <stdio.h>
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
    u_puts((const int8_t *)"  -run <file> [args]  compile and run directly\n");
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

static bool tcc_has_exact_arg(int argc, char **argv, const char *opt)
{
    int i;

    if (!opt || opt[0] == '\0')
    {
        return false;
    }

    for (i = 1; i < argc; i++)
    {
        if (!argv[i])
        {
            continue;
        }
        if (u_strcmp((const int8_t *)argv[i], (const int8_t *)opt) == 0)
        {
            return true;
        }
    }

    return false;
}

static int tcc_try_exec_real(int argc, char **argv)
{
    const int8_t *exec_argv[192];
    bool link_mode;
    bool want_static;
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
    want_static = link_mode && !tcc_has_exact_arg(argc, argv, "-static");
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
    exec_argv[outc++] = (const int8_t *)"-L";
    exec_argv[outc++] = (const int8_t *)"/usr/local/lib/tcc";
    exec_argv[outc++] = (const int8_t *)"-isystem";
    exec_argv[outc++] = (const int8_t *)"/usr/include";
    if (want_static)
    {
        /*
         * 关键兼容修复（中文）：
         * 当前内核 exec 仅支持“直接装载静态 ELF”路径，不支持动态解释器/动态重定位。
         * tcc 默认会生成带动态装载特征的可执行文件（多 PHDR），在内核里表现为“可运行但无输出”。
         * 这里在链接阶段默认强制 -static，确保产物可被内核稳定执行。
         */
        exec_argv[outc++] = (const int8_t *)"-static";
    }
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

/*
 * `tcc -run` 兼容实现（中文）：
 * upstream tcc 的 -run 依赖 runmain.o/libtcc1.a/宿主 libc 布局，
 * 在当前最小系统里会报 “runmain.o not found / library c not found”。
 *
 * 这里做成本地两段式：
 * 1) 先把源文件编译并静态链接成 /tmp 下临时 ELF；
 * 2) 再通过内核 exec 运行该 ELF，并透传 run 参数。
 */
static int tcc_try_run_mode(int argc, char **argv)
{
    const int8_t *compile_argv[224];
    const int8_t *run_argv[96];
    char tmp_out[96];
    int run_opt_idx;
    int src_idx;
    int outc;
    int rargc;
    int i;
    int64_t pid;
    int64_t waited;
    bool has_runtime_libs;
    bool has_libgcc;
    int rc;

    run_opt_idx = -1;
    for (i = 1; i < argc; i++)
    {
        if (!argv[i])
        {
            continue;
        }
        if (u_strcmp((const int8_t *)argv[i], (const int8_t *)"-run") == 0)
        {
            run_opt_idx = i;
            break;
        }
    }
    if (run_opt_idx < 0)
    {
        return -EINVAL;
    }
    if (run_opt_idx + 1 >= argc || !argv[run_opt_idx + 1] || argv[run_opt_idx + 1][0] == '\0')
    {
        u_puts((const int8_t *)"tcc: -run expects a source file\n");
        return -EINVAL;
    }

    src_idx = run_opt_idx + 1;
    has_runtime_libs = (access("/usr/lib/crt0.o", F_OK) == 0 &&
                        access("/usr/lib/libstupidos.a", F_OK) == 0);
    has_libgcc = (access("/usr/lib/libgcc.a", F_OK) == 0);
    if (!has_runtime_libs)
    {
        u_puts((const int8_t *)"tcc: -run requires /usr/lib/crt0.o and /usr/lib/libstupidos.a\n");
        return -ENOENT;
    }

    if (snprintf(tmp_out, sizeof(tmp_out), "/tmp/tcc-run-%d.elf", getpid()) <= 0)
    {
        return -EINVAL;
    }

    outc = 0;
    compile_argv[outc++] = (const int8_t *)"tcc";
    compile_argv[outc++] = (const int8_t *)"-B";
    compile_argv[outc++] = (const int8_t *)"/usr/local/lib/tcc";
    compile_argv[outc++] = (const int8_t *)"-L";
    compile_argv[outc++] = (const int8_t *)"/usr/local/lib/tcc";
    compile_argv[outc++] = (const int8_t *)"-isystem";
    compile_argv[outc++] = (const int8_t *)"/usr/include";
    compile_argv[outc++] = (const int8_t *)"-static";

    for (i = 1; i < run_opt_idx; i++)
    {
        if (!argv[i])
        {
            continue;
        }
        if (outc >= (int)(sizeof(compile_argv) / sizeof(compile_argv[0])) - 1)
        {
            return -E2BIG;
        }
        compile_argv[outc++] = (const int8_t *)argv[i];
    }

    compile_argv[outc++] = (const int8_t *)argv[src_idx];
    compile_argv[outc++] = (const int8_t *)"-o";
    compile_argv[outc++] = (const int8_t *)tmp_out;
    compile_argv[outc++] = (const int8_t *)"-nostdlib";
    compile_argv[outc++] = (const int8_t *)"/usr/lib/crt0.o";
    compile_argv[outc++] = (const int8_t *)"/usr/lib/libstupidos.a";
    if (has_libgcc)
    {
        compile_argv[outc++] = (const int8_t *)"/usr/lib/libgcc.a";
    }

    pid = u_exec((const int8_t *)"/bin/tcc.real", outc, compile_argv);
    if (pid < 0)
    {
        return (int)pid;
    }
    waited = u_waitpid((int32_t)pid);
    if (waited < 0)
    {
        return (int)waited;
    }

    if (access(tmp_out, F_OK) != 0)
    {
        u_puts((const int8_t *)"tcc: -run compile failed\n");
        return -ENOENT;
    }

    rargc = 0;
    run_argv[rargc++] = (const int8_t *)tmp_out;
    for (i = src_idx + 1; i < argc; i++)
    {
        if (!argv[i])
        {
            continue;
        }
        if (rargc >= (int)(sizeof(run_argv) / sizeof(run_argv[0])) - 1)
        {
            (void)unlink(tmp_out);
            return -E2BIG;
        }
        run_argv[rargc++] = (const int8_t *)argv[i];
    }

    pid = u_exec((const int8_t *)tmp_out, rargc, run_argv);
    if (pid < 0)
    {
        (void)unlink(tmp_out);
        return (int)pid;
    }
    waited = u_waitpid((int32_t)pid);
    rc = (waited < 0) ? (int)waited : 0;
    (void)unlink(tmp_out);
    return rc;
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

        if ((u_strcmp((const int8_t *)argv[i], (const int8_t *)"-v") == 0 ||
             u_strcmp((const int8_t *)argv[i], (const int8_t *)"--version") == 0) &&
            argc == 2)
        {
            u_puts((const int8_t *)"tcc (stupidos wrapper) + tinycc backend\n");
            u_puts((const int8_t *)"backend path: /bin/tcc.real\n");
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
        if (tcc_has_exact_arg(argc, argv, "-run"))
        {
            ret = tcc_try_run_mode(argc, argv);
            if (ret == 0)
            {
                return 0;
            }
            u_puts((const int8_t *)"tcc: -run fallback to direct tcc.real\n");
        }

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
