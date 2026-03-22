#include "stupidos_user.h"

static uint32_t sleep_parse_u32(const int8_t *str)
{
    uint64_t value;
    size_t i;

    if (!str || !*str)
    {
        return 0;
    }

    value = 0;
    for (i = 0; str[i] != '\0'; i++)
    {
        if (str[i] < '0' || str[i] > '9')
        {
            return 0;
        }

        value = value * 10ULL + (uint64_t)(str[i] - '0');
        if (value > 0xffffffffULL)
        {
            return 0;
        }
    }

    return (uint32_t)value;
}

static void sleep_usage(void)
{
    u_puts((const int8_t *)"usage: sleep [seconds]\n");
    u_puts((const int8_t *)"       sleep -m <milliseconds>\n");
}

int main(int argc, char **argv)
{
    uint32_t ms;
    uint32_t secs;
    int i;

    ms = 0;
    secs = 0;

    if (argc <= 1)
    {
        ms = 1000U;
    }
    else if (u_strcmp((const int8_t *)argv[1], (const int8_t *)"-m") == 0)
    {
        if (argc != 3)
        {
            sleep_usage();
            return 1;
        }

        ms = sleep_parse_u32((const int8_t *)argv[2]);
        if (!ms && argv[2][0] != '0')
        {
            sleep_usage();
            return 1;
        }
    }
    else
    {
        for (i = 1; i < argc; i++)
        {
            if (!argv[i])
            {
                continue;
            }

            secs = sleep_parse_u32((const int8_t *)argv[i]);
            if (!secs && argv[i][0] != '0')
            {
                sleep_usage();
                return 1;
            }
        }

        ms = secs * 1000U;
    }

    /*
     * 这里直接睡到时间到。
     * 交给内核的 sleep syscall 之后，调度器不会被用户态轮询打扰。
     */
    if (u_sleep_ms(ms) < 0)
    {
        return 1;
    }

    return 0;
}
