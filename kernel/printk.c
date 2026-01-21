#include "printk.h"

#define is_digit(c) ((c) >= '0' && (c) <= '9')

static int8_t *number(int8_t *str, int64_t num, int32_t base, int32_t size, int32_t precision, int32_t type)
{
    int8_t c, sign, tmp[50];
    const int8_t *digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int32_t i;

    if (type & SMALL)
    {
        digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    }
    if (type & LEFT)
    {
        type &= ~ZEROPAD;
    }
    if (base < 2 || base > 36)
    {
        return 0;
    }
    c = (type & ZEROPAD) ? '0' : ' ';
    sign = 0;
    if (type & SIGN && num < 0)
    {
        sign = '-';
        num = -num;
    }
    else
    {
        sign = (type & PLUS) ? '+' : ((type & SPACE) ? ' ' : 0);
    }
    if (sign)
    {
        size--;
    }
    if (type & SPECIAL)
    {
        if (base == 16)
        {
            size -= 2;
        }
        else if (base == 8)
        {
            size--;
        }
    }
    i = 0;
    if (num == 0)
    {
        tmp[i++] = '0';
    }
    else
    {
        while (num != 0)
        {
            tmp[i++] = digits[num % base];
            num /= base;
        }
    }
    if (i > precision)
    {
        precision = i;
    }
    size -= precision;
    if (!(type & (ZEROPAD + LEFT)))
    {
        while (size-- > 0)
        {
            *str++ = ' ';
        }
    }
    if (sign)
    {
        *str++ = sign;
    }
    if (type & SPECIAL)
    {
        if (base == 8)
        {
            *str++ = '0';
        }
        else if (base == 16)
        {
            *str++ = '0';
            *str++ = digits[33];
        }
    }
    if (!(type & LEFT))
    {
        while (size-- > 0)
        {
            *str++ = c;
        }
    }

    while (i < precision--)
    {
        *str++ = '0';
    }
    while (i-- > 0)
    {
        *str++ = tmp[i];
    }
    while (size-- > 0)
    {
        *str++ = ' ';
    }
    return str;
}

static int32_t skip_atoi(const int8_t **s)
{
    int32_t i = 0;

    while (is_digit(**s))
    {
        i = i * 10 + *((*s)++) - '0';
    }
    return i;
}

int32_t vsprintf(int8_t *buf, const int8_t *fmt, va_list args)
{
    int8_t *str, *s;
    int32_t flags;
    int32_t field_width;
    int32_t precision;
    int32_t len, i;

    int32_t qualifier; /* 'h', 'l', 'L' or 'Z' for integer fields */

    for (str = buf; *fmt; fmt++)
    {

        if (*fmt != '%')
        {
            *str++ = *fmt;
            continue;
        }
        flags = 0;
    repeat:
        fmt++;
        switch (*fmt)
        {
        case '-':
            flags |= LEFT;
            goto repeat;
        case '+':
            flags |= PLUS;
            goto repeat;
        case ' ':
            flags |= SPACE;
            goto repeat;
        case '#':
            flags |= SPECIAL;
            goto repeat;
        case '0':
            flags |= ZEROPAD;
            goto repeat;
        }

        field_width = -1;
        if (is_digit(*fmt))
        {
            field_width = skip_atoi(&fmt);
        }
        else if (*fmt == '*')
        {
            fmt++;
            field_width = va_arg(args, int32_t);
            if (field_width < 0)
            {
                field_width = -field_width;
                flags |= LEFT;
            }
        }

        precision = -1;
        if (*fmt == '.')
        {
            fmt++;
            if (is_digit(*fmt))
            {
                precision = skip_atoi(&fmt);
            }
            else if (*fmt == '*')
            {
                fmt++;
                precision = va_arg(args, int32_t);
            }
            if (precision < 0)
            {
                precision = 0;
            }
        }

        qualifier = -1;
        if (*fmt == 'h' || *fmt == 'l' || *fmt == 'L' || *fmt == 'Z')
        {
            qualifier = *fmt;
            fmt++;
        }

        switch (*fmt)
        {
        case 'c':

            if (!(flags & LEFT))
            {
                while (--field_width > 0)
                {
                    *str++ = ' ';
                }
            }
            *str++ = (uint8_t)va_arg(args, int32_t);
            while (--field_width > 0)
            {
                *str++ = ' ';
            }
            break;

        case 's':

            s = va_arg(args, int8_t *);
            if (!s)
            {
                s = '\0';
            }
            len = strlen(s);
            if (precision < 0)
            {
                precision = len;
            }
            else if (len > precision)
            {
                len = precision;
            }

            if (!(flags & LEFT))
            {
                while (len < field_width--)
                {
                    *str++ = ' ';
                }
            }
            for (i = 0; i < len; i++)
            {
                *str++ = *s++;
            }
            while (len < field_width--)
            {
                *str++ = ' ';
            }
            break;

        case 'o':
            if (qualifier == 'l')
            {
                str = number(str, va_arg(args, uint64_t), 8, field_width, precision, flags);
            }
            else
            {
                str = number(str, va_arg(args, uint32_t), 8, field_width, precision, flags);
            }
            break;

        case 'p':

            if (field_width == -1)
            {
                field_width = 2 * sizeof(void *);
                flags |= ZEROPAD;
            }

            str = number(str, (uint64_t)va_arg(args, void *), 16, field_width, precision, flags);
            break;

        case 'x':

            flags |= SMALL;

        case 'X':

            if (qualifier == 'l')
            {
                str = number(str, va_arg(args, uint64_t), 16, field_width, precision, flags);
            }
            else
            {
                str = number(str, va_arg(args, uint32_t), 16, field_width, precision, flags);
            }
            break;

        case 'd':
        case 'i':
            flags |= SIGN;
        case 'u':
            if (qualifier == 'l')
            {
                str = number(str, va_arg(args, uint64_t), 10, field_width, precision, flags);
            }
            else
            {
                str = number(str, va_arg(args, uint32_t), 10, field_width, precision, flags);
            }
            break;

        case 'n':

            if (qualifier == 'l')
            {
                int64_t *ip = va_arg(args, int64_t *);
                *ip = (str - buf);
            }
            else
            {
                int32_t *ip = va_arg(args, int32_t *);
                *ip = (str - buf);
            }
            break;
        case 'b': // binary
            str = number(str, va_arg(args, uint64_t), 2, field_width, precision, flags);
            break;
        case 'm': // mac address
            flags |= SMALL | ZEROPAD;
            uint8_t *ptr = va_arg(args, uint8_t *);
            for (uint32_t t = 0; t < 6; t++, ptr++)
            {
                int num = *ptr;
                str = number(str, num, 16, 2, precision, flags);
                *str = ':';
                str++;
            }
            str--;
            break;
        case 'r': // ip address
            flags |= SMALL;
            ptr = va_arg(args, uint8_t *);
            for (uint32_t t = 0; t < 4; t++, ptr++)
            {
                uint32_t num = *ptr;
                str = number(str, num, 10, field_width, precision, flags);
                *str = '.';
                str++;
            }
            str--;
            break;
        case '%':
            *str++ = '%';
            break;

        default:
            *str++ = '%';
            if (*fmt)
            {
                *str++ = *fmt;
            }
            else
            {
                fmt--;
            }
            break;
        }
    }
    *str = '\0';
    return (str - buf);
}

int32_t sprintf(int8_t *buffer, const int8_t *fmt, ...)
{
    int32_t i = 0;

    va_list args;
    va_start(args, fmt);
    i = vsprintf(buffer, fmt, args);
    va_end(args);

    return i;
}
