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
