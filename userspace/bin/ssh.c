#include "stupidos_user.h"
#include <string.h>

/*
 * ssh 的用户体验优先封装。
 *
 * 这里不直接暴露 Dropbear 的所有内部细节，而是把它当作一个
 * “更容易连上”的薄包装：
 * - 默认追加 `-y`，让未知主机密钥直接接受
 * - 继续把用户原始参数原样传给 dbclient
 *
 * 这样既保留了 Dropbear 客户端的完整 SSH 协议栈，
 * 又避免了第一次连接时因为 hostkey 交互卡住。
 */

static int ssh_has_short_option(int argc, char **argv, char option_char)
{
    int i;

    for (i = 1; i < argc; i++)
    {
        if (argv[i] && argv[i][0] == '-' && argv[i][1] == option_char)
        {
            return 1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    const int8_t *dbclient_argv[64];
    int outc;
    int i;
    bool host_seen;
    int64_t pid;

    outc = 0;
    dbclient_argv[outc++] = (const int8_t *)"dbclient";
    /*
     * Dropbear 的 dbclient 已经支持 `-y` / `-o StrictHostKeyChecking=...`。
     * 这里保持尽量“像原生 ssh”：
     * - 只默认接受未知 hostkey，避免第一次连接卡在确认提示
     * - 不额外强推 BatchMode / PasswordAuthentication，减少和后续认证
     *   流程的冲突面
     */
    dbclient_argv[outc++] = (const int8_t *)"-y";
    host_seen = false;

    /*
     * 如果用户已经显式给了 `-l user`，就不要再把 `[user@]host` 拆成额外的
     * `-l`，避免用户名重复覆盖。
     */
    if (ssh_has_short_option(argc, argv, 'l'))
    {
        host_seen = true;
    }
    for (i = 1; i < argc && outc < (int)(sizeof(dbclient_argv) / sizeof(dbclient_argv[0])) - 1; i++)
    {
        const char *arg;
        bool takes_value;

        arg = argv[i];
        takes_value = false;
        if (arg[0] == '-' && arg[1] != '\0' && arg[2] == '\0')
        {
            switch (arg[1])
            {
            case 'o':
            case 'p':
            case 'l':
            case 'i':
            case 'W':
            case 'K':
            case 'I':
            case 'J':
            case 'b':
            case 'L':
            case 'R':
            case 'B':
            case 'c':
            case 'm':
                takes_value = true;
                break;
            default:
                break;
            }
        }

        if (takes_value)
        {
            dbclient_argv[outc++] = (const int8_t *)arg;
            if ((i + 1) < argc)
            {
                dbclient_argv[outc++] = (const int8_t *)argv[++i];
            }
            continue;
        }

        if (!host_seen && arg[0] != '-')
        {
            host_seen = true;
            /*
             * dbclient 原生就支持 `[user@]host` 语法。
             * 这里保持原样直传，避免我们自己拆分后把用户名/主机
             * 的提示信息弄得过于别扭。
             */
        }

        dbclient_argv[outc++] = (const int8_t *)arg;
    }
    dbclient_argv[outc] = NULL;

    pid = u_exec((const int8_t *)"/bin/dbclient", outc, dbclient_argv);
    if (pid < 0)
    {
        return 1;
    }

    (void)u_waitpid((int32_t)pid);
    return 0;
}
