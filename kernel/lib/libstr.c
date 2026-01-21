#include "lib/libstr.h"

int32_t strlen(int8_t *str)
{
    uint32_t res = 0;
    for(res = 0; str[res] != '\0'; res++);
    return res;
}