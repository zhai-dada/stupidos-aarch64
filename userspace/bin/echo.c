#include "stupidos_user.h"

/*
 * /bin/echo
 *
 * 目标：让 echo 成为独立 ELF，可被 shell 直接 exec，
 * 行为尽量贴近常见 Linux echo：
 * - 支持 -n（不输出末尾换行）
 * - 支持 -e（解析常见转义字符）
 */

static void echo_putc(uint8_t ch)
{
    (void)u_write(STUPIDOS_STDOUT_FILENO, &ch, 1);
}

static void echo_puts_raw(const int8_t *s)
{
    size_t len;

    if (!s)
    {
        return;
    }

    len = u_strnlen(s, 4096);
    if (len)
    {
        (void)u_write(STUPIDOS_STDOUT_FILENO, s, len);
    }
}

static void echo_puts_escaped(const int8_t *s)
{
    size_t i;

    if (!s)
    {
        return;
    }

    for (i = 0; s[i] != '\0'; i++)
    {
        if (s[i] == '\\' && s[i + 1] != '\0')
        {
            i++;
            switch (s[i])
            {
            case 'n':
                echo_putc('\n');
                break;
            case 'r':
                echo_putc('\r');
                break;
            case 't':
                echo_putc('\t');
                break;
            case '\\':
                echo_putc('\\');
                break;
            case '0':
                echo_putc('\0');
                break;
            default:
                echo_putc('\\');
                echo_putc((uint8_t)s[i]);
                break;
            }
            continue;
        }

        echo_putc((uint8_t)s[i]);
    }
}

int main(int argc, char **argv)
{
    int i;
    bool newline;
    bool enable_escape;

    newline = true;
    enable_escape = false;
    i = 1;

    while (i < argc && argv[i])
    {
        if (u_strcmp((const int8_t *)argv[i], (const int8_t *)"-n") == 0)
        {
            newline = false;
            i++;
            continue;
        }

        if (u_strcmp((const int8_t *)argv[i], (const int8_t *)"-e") == 0)
        {
            enable_escape = true;
            i++;
            continue;
        }

        if (u_strcmp((const int8_t *)argv[i], (const int8_t *)"--") == 0)
        {
            i++;
            break;
        }

        break;
    }

    for (; i < argc; i++)
    {
        if (argv[i])
        {
            if (enable_escape)
            {
                echo_puts_escaped((const int8_t *)argv[i]);
            }
            else
            {
                echo_puts_raw((const int8_t *)argv[i]);
            }
        }

        if (i + 1 < argc)
        {
            echo_putc(' ');
        }
    }

    if (newline)
    {
        echo_putc('\n');
    }

    return 0;
}
