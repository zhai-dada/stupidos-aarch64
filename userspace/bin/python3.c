#include <Python.h>
#include <stdlib.h>

/*
 * CPython 用户态入口（中文）：
 * 先直接复用官方初始化路径，后续再按系统能力裁剪 site/importlib 等流程。
 */
int main(int argc, char **argv)
{
    /*
     * 路径自举（中文）：
     * 在最小系统里默认不存在宿主机那套 /usr/local 自动探测上下文，
     * 这里给一个稳定的默认 PYTHONHOME，避免每次启动都走噪声告警路径。
     */
    if (!getenv("PYTHONHOME"))
    {
        (void)setenv("PYTHONHOME", "/usr/local", 1);
    }

    return Py_BytesMain(argc, argv);
}
