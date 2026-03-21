#include "lib/libstr.h"

size_t strlen(int8_t *str)
{
    size_t res = 0;
    for(res = 0; str[res] != '\0'; res++);
    return res;
}

size_t strcmp(const int8_t *str1, const int8_t *str2)
{
    while (*str1 && *str2 && *str1 == *str2)
    {
        str1++;
        str2++;
    }
    return *str1 - *str2;
}

size_t strncmp(const int8_t *str1, const int8_t *str2, size_t n)
{
    while (n && *str1 && *str2 && *str1 == *str2)
    {
        str1++;
        str2++;
        n--;
    }

    if (n == 0)
    {
        return 0;
    }

    return *str1 - *str2;
}

int32_t memcmp(const void *s1, const void *s2, size_t n)
{
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;

    while (n--)
    {
        if (*p1 != *p2)
        {
            return *p1 - *p2;
        }

        p1++;
        p2++;
    }

    return 0;
}
