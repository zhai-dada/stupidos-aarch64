#include "stupidos_user.h"

static int parse_ipv4(const int8_t *str, uint32_t *out)
{
    uint32_t octets[4];
    uint32_t cur;
    size_t idx;
    size_t i;

    if (!str || !out)
    {
        return -1;
    }

    idx = 0;
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
            if (idx >= 4)
            {
                return -1;
            }
            octets[idx++] = cur;
            cur = 0;
            if (ch == '\0')
            {
                break;
            }
            continue;
        }

        return -1;
    }

    if (idx != 4)
    {
        return -1;
    }

    *out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return 0;
}

static void usage(void)
{
    u_puts((const int8_t *)"usage: netcfg <ip> <netmask> <gateway>\n");
    u_puts((const int8_t *)"example: netcfg 10.0.2.15 255.255.255.0 10.0.2.2\n");
}

int main(int argc, char **argv)
{
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    int ret;

    if (argc != 4)
    {
        usage();
        return 1;
    }

    ret = parse_ipv4((const int8_t *)argv[1], &ip);
    if (ret)
    {
        usage();
        return 1;
    }

    ret = parse_ipv4((const int8_t *)argv[2], &netmask);
    if (ret)
    {
        usage();
        return 1;
    }

    ret = parse_ipv4((const int8_t *)argv[3], &gateway);
    if (ret)
    {
        usage();
        return 1;
    }

    ret = (int)u_netcfg(ip, netmask, gateway);
    if (ret < 0)
    {
        u_puts((const int8_t *)"netcfg: failed\n");
        return 1;
    }

    u_puts((const int8_t *)"netcfg: ok\n");
    return 0;
}
