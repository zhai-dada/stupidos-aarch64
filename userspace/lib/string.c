#include "stupidos_user.h"

size_t u_strlen(const int8_t *str)
{
    return u_strnlen(str, (size_t)-1);
}

size_t u_strnlen(const int8_t *str, size_t max_len)
{
    size_t len;

    len = 0;
    /*
     * 用户态和内核当前还共享同一套地址空间。
     * 一旦字符串指针被污染成 0x1c 这类低地址，继续逐字节读只会直接进异常。
     * 这里先做最小防御：空指针和低地址指针一律视为无效字符串。
     */
    if (!str || (uint64_t)str < 0x1000UL || !max_len)
    {
        return 0;
    }

    while (len < max_len && str[len] != '\0')
    {
        len++;
    }

    return len;
}

int u_strcmp(const int8_t *a, const int8_t *b)
{
    if (!a || !b || (uint64_t)a < 0x1000UL || (uint64_t)b < 0x1000UL)
    {
        if (a == b)
        {
            return 0;
        }

        return (!a || (uint64_t)a < 0x1000UL) ? -1 : 1;
    }

    while (*a && *b && *a == *b)
    {
        a++;
        b++;
    }

    return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

void *u_memcpy(void *dst, const void *src, size_t len)
{
    uint8_t *d;
    const uint8_t *s;
    size_t i;

    d = (uint8_t *)dst;
    s = (const uint8_t *)src;
    for (i = 0; i < len; i++)
    {
        d[i] = s[i];
    }

    return dst;
}

void *u_memset(void *dst, int value, size_t len)
{
    uint8_t *d;
    size_t i;

    d = (uint8_t *)dst;
    for (i = 0; i < len; i++)
    {
        d[i] = (uint8_t)value;
    }

    return dst;
}
