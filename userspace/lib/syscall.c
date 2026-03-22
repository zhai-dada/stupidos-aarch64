#include "stupidos_user.h"

void u_putsn(const int8_t *str, size_t len)
{
    if (!str || !len || (uint64_t)str < 0x1000UL)
    {
        return;
    }

    (void)u_write(STUPIDOS_STDOUT_FILENO, str, len);
}

void u_puts(const int8_t *str)
{
    if (!str)
    {
        return;
    }

    /*
     * 这里不要盲目相信上层传下来的指针。
     * shell 这种早期用户态程序一旦把字符串地址搞坏，直接 strlen() 会把整机拉进异常。
     */
    u_putsn(str, u_strnlen(str, 4096));
}

void u_putc(int8_t ch)
{
    (void)u_write(STUPIDOS_STDOUT_FILENO, &ch, 1);
}
