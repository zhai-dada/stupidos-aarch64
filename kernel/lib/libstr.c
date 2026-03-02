#include "lib/libstr.h"

size_t strlen(int8_t *str)
{
    size_t res = 0;
    for(res = 0; str[res] != '\0'; res++);
    return res;
}