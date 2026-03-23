#ifndef __STUPIDOS_STDIO_H__
#define __STUPIDOS_STDIO_H__

#include_next <stdio.h>

/*
 * 这里不重新定义 FILE 结构，避免和工具链自带头文件冲突。
 * 我们只把高频的字符输入输出宏重定向到 stupidos 的实现。
 */
#define getc(fp)              fgetc(fp)
#define putc(ch, fp)          fputc((ch), (fp))
#define getchar()             fgetc(stdin)
#define putchar(ch)           fputc((ch), stdout)
#define getc_unlocked(fp)     fgetc(fp)
#define putc_unlocked(ch, fp) fputc((ch), (fp))

#endif
