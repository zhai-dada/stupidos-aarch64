#include "stupidos_user.h"

#define PING_DEFAULT_COUNT        4
#define PING_DEFAULT_INTERVAL_MS  1000
#define PING_DEFAULT_TIMEOUT_MS   3000
#define PING_PAYLOAD_BYTES        56
static void ping_puts(const int8_t *str)
{
    if (!str)
    {
        return;
    }

    u_puts(str);
}

static void ping_putc(char ch)
{
    u_putc((int8_t)ch);
}

static void ping_put_u64(uint64_t value)
{
    int8_t buf[32];
    size_t pos;

    pos = sizeof(buf);
    buf[--pos] = '\0';
    if (value == 0)
    {
        buf[--pos] = '0';
        ping_puts(&buf[pos]);
        return;
    }

    while (value && pos > 0)
    {
        buf[--pos] = (int8_t)('0' + (value % 10ULL));
        value /= 10ULL;
    }

    ping_puts(&buf[pos]);
}

static void ping_put_u32(uint32_t value)
{
    ping_put_u64((uint64_t)value);
}

static void ping_put_padded_u32(uint32_t value, uint32_t width)
{
    int8_t buf[16];
    uint32_t pos;
    uint32_t digits;

    pos = sizeof(buf);
    buf[--pos] = '\0';
    digits = 0;
    if (value == 0)
    {
        buf[--pos] = '0';
        digits = 1;
    }
    else
    {
        while (value && pos > 0)
        {
            buf[--pos] = (int8_t)('0' + (value % 10U));
            value /= 10U;
            digits++;
        }
    }

    while (digits < width && pos > 0)
    {
        buf[--pos] = '0';
        digits++;
    }

    ping_puts(&buf[pos]);
}

static void ping_put_ip(uint32_t ip)
{
    ping_put_u32((ip >> 24) & 0xffU);
    ping_putc('.');
    ping_put_u32((ip >> 16) & 0xffU);
    ping_putc('.');
    ping_put_u32((ip >> 8) & 0xffU);
    ping_putc('.');
    ping_put_u32(ip & 0xffU);
}

static void ping_put_ms_value(uint64_t usec)
{
    uint64_t ms;
    uint64_t frac;

    ms = usec / 1000ULL;
    frac = usec % 1000ULL;
    ping_put_u64(ms);
    ping_putc('.');
    ping_put_padded_u32((uint32_t)frac, 3);
}

static int ping_parse_u32(const int8_t *str, uint32_t *out)
{
    uint64_t value;
    size_t i;

    if (!str || !out || !*str)
    {
        return -1;
    }

    value = 0;
    for (i = 0; str[i] != '\0'; i++)
    {
        if (str[i] < '0' || str[i] > '9')
        {
            return -1;
        }

        value = value * 10ULL + (uint64_t)(str[i] - '0');
        if (value > 0xffffffffULL)
        {
            return -1;
        }
    }

    *out = (uint32_t)value;
    return 0;
}

static int ping_parse_ipv4(const int8_t *str, uint32_t *out)
{
    uint32_t octets[4];
    uint32_t cur;
    size_t octet_idx;
    size_t i;

    if (!str || !out)
    {
        return -1;
    }

    octet_idx = 0;
    cur = 0;
    for (i = 0; ; i++)
    {
        char ch = (char)str[i];

        if (ch >= '0' && ch <= '9')
        {
            cur = cur * 10U + (uint32_t)(ch - '0');
            if (cur > 255U)
            {
                return -1;
            }
            continue;
        }

        if (ch == '.' || ch == '\0')
        {
            if (octet_idx >= 4)
            {
                return -1;
            }
            octets[octet_idx++] = cur;
            cur = 0;

            if (ch == '\0')
            {
                break;
            }
            continue;
        }

        return -1;
    }

    if (octet_idx != 4)
    {
        return -1;
    }

    *out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return 0;
}

static void ping_sleep_ms(uint32_t ms)
{
    if (!ms)
    {
        return;
    }

    /*
     * 现在直接走内核 sleep syscall。
     * 这样 ping 的等待不会再靠用户态主动 yield，
     * shell 和其他前台交互任务会更顺滑。
     */
    (void)u_sleep_ms(ms);
}

static void ping_usage(void)
{
    ping_puts((const int8_t *)"usage: ping [-c count] [-i interval] <ipv4>\n");
    ping_puts((const int8_t *)"  -c count     number of probes, default 4\n");
    ping_puts((const int8_t *)"  -i interval  interval in seconds, default 1\n");
}

int main(int argc, char **argv)
{
    uint32_t target_ip;
    uint32_t count;
    uint32_t interval_ms;
    uint32_t timeout_ms;
    uint32_t transmitted;
    uint32_t received;
    uint64_t sum_us;
    uint64_t min_us;
    uint64_t max_us;
    int64_t ret;
    uint32_t seq;
    int i;

    target_ip = 0;
    count = PING_DEFAULT_COUNT;
    interval_ms = PING_DEFAULT_INTERVAL_MS;
    timeout_ms = PING_DEFAULT_TIMEOUT_MS;

    for (i = 1; i < argc; i++)
    {
        if (!argv[i])
        {
            continue;
        }

        if (u_strcmp((const int8_t *)argv[i], (const int8_t *)"-c") == 0)
        {
            if (i + 1 >= argc || !argv[i + 1] || ping_parse_u32((const int8_t *)argv[i + 1], &count))
            {
                ping_usage();
                return 1;
            }
            i++;
            continue;
        }

        if (u_strcmp((const int8_t *)argv[i], (const int8_t *)"-i") == 0)
        {
            uint32_t interval_s;

            if (i + 1 >= argc || !argv[i + 1] || ping_parse_u32((const int8_t *)argv[i + 1], &interval_s))
            {
                ping_usage();
                return 1;
            }
            interval_ms = interval_s * 1000U;
            i++;
            continue;
        }

        if (argv[i][0] == '-')
        {
            ping_usage();
            return 1;
        }

        if (target_ip)
        {
            ping_usage();
            return 1;
        }

        if (ping_parse_ipv4((const int8_t *)argv[i], &target_ip))
        {
            ping_usage();
            return 1;
        }
    }

    if (!target_ip)
    {
        ping_usage();
        return 1;
    }

    if (!count)
    {
        count = PING_DEFAULT_COUNT;
    }

    ping_puts((const int8_t *)"PING ");
    ping_put_ip(target_ip);
    ping_putc(' ');
    ping_putc('(');
    ping_put_ip(target_ip);
    ping_puts((const int8_t *)"): ");
    ping_put_u32(PING_PAYLOAD_BYTES);
    ping_puts((const int8_t *)" data bytes\n");

    transmitted = 0;
    received = 0;
    sum_us = 0;
    min_us = (uint64_t)-1;
    max_us = 0;

    for (seq = 1; seq <= count; seq++)
    {
        transmitted++;
        ret = u_netping(target_ip, (uint16_t)seq, timeout_ms);
        if (ret < 0)
        {
            if (ret == -STUPIDOS_EHOSTUNREACH)
            {
                ping_puts((const int8_t *)"From ");
                ping_put_ip(target_ip);
                ping_puts((const int8_t *)" icmp_seq=");
                ping_put_u32(seq);
                ping_puts((const int8_t *)" Destination Host Unreachable\n");
            }
            else if (ret == -STUPIDOS_ETIMEDOUT)
            {
                ping_puts((const int8_t *)"Request timeout for icmp_seq ");
                ping_put_u32(seq);
                ping_puts((const int8_t *)"\n");
            }
            else
            {
                ping_puts((const int8_t *)"ping: error ");
                ping_put_u64((uint64_t)(-ret));
                ping_puts((const int8_t *)" for icmp_seq ");
                ping_put_u32(seq);
                ping_puts((const int8_t *)"\n");
            }
        }
        else
        {
            received++;
            sum_us += (uint64_t)ret;
            if ((uint64_t)ret < min_us)
            {
                min_us = (uint64_t)ret;
            }
            if ((uint64_t)ret > max_us)
            {
                max_us = (uint64_t)ret;
            }

            ping_put_u32(PING_PAYLOAD_BYTES + 8);
            ping_puts((const int8_t *)" bytes from ");
            ping_put_ip(target_ip);
            ping_puts((const int8_t *)": icmp_seq=");
            ping_put_u32(seq);
            ping_puts((const int8_t *)" ttl=64 time=");
            ping_put_ms_value((uint64_t)ret);
            ping_puts((const int8_t *)" ms");
            ping_puts((const int8_t *)"\n");
        }

        if (seq < count)
        {
            ping_sleep_ms(interval_ms);
        }
    }

    ping_puts((const int8_t *)"\n--- ");
    ping_put_ip(target_ip);
    ping_puts((const int8_t *)" ping statistics ---\n");
    ping_put_u32(transmitted);
    ping_puts((const int8_t *)" packets transmitted, ");
    ping_put_u32(received);
    ping_puts((const int8_t *)" received, ");
    if (transmitted == 0)
    {
        ping_puts((const int8_t *)"0% packet loss\n");
    }
    else
    {
        ping_put_u32((uint32_t)(((transmitted - received) * 100U) / transmitted));
        ping_puts((const int8_t *)"% packet loss\n");
    }

    if (received)
    {
        ping_puts((const int8_t *)"rtt min/avg/max = ");
        ping_put_ms_value(min_us);
        ping_putc('/');
        ping_put_ms_value(sum_us / received);
        ping_putc('/');
        ping_put_ms_value(max_us);
        ping_puts((const int8_t *)" ms\n");
    }

    return received ? 0 : 1;
}
