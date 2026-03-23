#include "stupidos_user.h"
#include "Python.h"

/*
 * stupidos 版 Python3 包装器。
 *
 * 这里不去依赖宿主系统的动态链接器，而是直接调用 libpython3.10.a
 * 里的入口，把它当作目标系统里的普通用户态程序来运行。
 *
 * 这样做的好处：
 * - 和当前 OS 的 exec 模型一致
 * - 不需要先补完整动态加载器
 * - 后续只要用户态兼容层继续补系统调用，Python 能逐步长起来
 */
int main(int argc, char **argv)
{
    /*
     * 明确告诉 CPython 它的标准库基路径，避免 getpath 阶段把系统路径
     * 误判成宿主环境。
     */
    (void)setenv("PYTHONHOME", "/usr/local", 1);
    (void)setenv("PYTHONUTF8", "1", 1);
    return Py_BytesMain(argc, argv);
}
