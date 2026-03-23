/*
 * stupidos 最小 CPython 构建占位文件。
 * 我们先把动态加载入口收口为一个空实现，避免解释器启动时依赖宿主
 * 系统的 dlopen / dlsym / dlerror 这条链。
 */

#include "Python.h"
#include "importdl.h"

/*
 * 这里显式提供 import.c / importdl.c 需要的动态加载入口符号。
 * 返回空表和空函数指针，表示当前构建不支持扩展模块加载。
 */
const char *_PyImport_DynLoadFiletab[] = {
    NULL,
};

dl_funcptr _PyImport_FindSharedFuncptr(const char *prefix,
                                       const char *shortname,
                                       const char *pathname,
                                       FILE *fp)
{
    (void)prefix;
    (void)shortname;
    (void)pathname;
    (void)fp;
    return NULL;
}
