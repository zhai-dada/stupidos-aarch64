#include "lib/libmem.h"

static void *__memset(int8_t *s, int32_t c, size_t count)
{
    int8_t *p = s;
    uint32_t align = 16;
    uint64_t c_8byte = 0, size = count;
    uint64_t address = (uint64_t)p;

    if (address & (align - 1))
    {
        int32_t unaligned_count = (address & (align - 1));
        if (unaligned_count > count)
        {
            __memset_1bytes(p, c, count);
        }
        else
        {
            __memset_1bytes(p, c, unaligned_count);
            p += unaligned_count;
            size -= unaligned_count;
        }
    }
    if (size > align)
    {
        if (c != 0)
        {
            for (int n = 0; n < 8; n++)
            {
                c_8byte |= ((c & 0xff) << (n * 8));
            }
        }

        __memset_16bytes(p, c_8byte, size / align);
        p += align * (size / align);

        size = size % align;
    }
    if (size)
    {
        __memset_1bytes(p, c, size);
    }
    return s;
}

void *memset(int8_t *s, int32_t c, size_t count)
{
    if (count)
    {
        __memset(s, c, count);
    }
    return s;
}
