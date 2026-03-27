#include "stupidos_user.h"

/*
 * vim 作为 vi 的薄包装器。
 *
 * BusyBox 的 vi 已经提供了编辑器本体，
 * 这里保留一个传统的 vim 入口，方便用户习惯上直接输入 vim。
 */

int main(int argc, char **argv)
{
    int pid;

    pid = u_exec((const int8_t *)"/bin/vi", argc, (const int8_t **)argv);
    if (pid < 0)
    {
        return 1;
    }

    (void)u_waitpid((int32_t)pid);
    return 0;
}
