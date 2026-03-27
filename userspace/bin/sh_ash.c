#include <stdlib.h>
#include "busybox.h"

/*
 * BusyBox ash 入口。
 *
 * 这里把 upstream 的 ash / lineedit / shell_common 直接接进 stupidos
 * 用户态库，目标是先把“能用、像 Linux 的 /bin/sh”跑起来，再继续补全。
 */

/*
 * ash 依赖 BusyBox 的全局指针技巧，这些 ptr hack 文件只提供符号，
 * 不会引入完整 BusyBox 运行时。
 */
#include "../../third_party/busybox/shell/ash_ptr_hack.c"
#include "../../third_party/busybox/shell/shell_common.c"
#include "../../third_party/busybox/libbb/lineedit_ptr_hack.c"
#include "../../third_party/busybox/libbb/lineedit.c"
#include "../../third_party/busybox/shell/ash.c"

int main(int argc, char **argv)
{
    /*
     * 现在默认直接进入 ash。
     * 如果后面需要对比旧 shell 的行为，可以在需要时把旧实现
     * 单独恢复成另一个 applet，不影响这里的入口设计。
     */
    return ash_main(argc, argv);
}
