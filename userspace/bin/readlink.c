#include "stupidos_user.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int8_t buf[STUPIDOS_PATH_MAX];
    ssize_t n;

    if (argc != 2 || !argv[1] || argv[1][0] == '\0')
    {
        u_puts((const int8_t *)"usage: readlink <path>\n");
        return 1;
    }

    u_memset(buf, 0, sizeof(buf));
    n = u_readlink((const int8_t *)argv[1], buf, sizeof(buf) - 1U);
    if (n < 0)
    {
        u_puts((const int8_t *)"readlink: ");
        u_puts((const int8_t *)argv[1]);
        u_puts((const int8_t *)": ");
        u_puts((const int8_t *)strerror(errno));
        u_puts((const int8_t *)"\n");
        return 1;
    }

    buf[n] = '\0';
    u_puts(buf);
    u_puts((const int8_t *)"\n");
    return 0;
}
