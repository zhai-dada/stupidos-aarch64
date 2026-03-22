#include "stupidos_user.h"

#define CAT_BUF_SIZE 128

int main(int argc, char **argv)
{
    int fd;
    ssize_t nread;
    int8_t buf[CAT_BUF_SIZE];

    if (argc < 2 || !argv[1])
    {
        u_puts((const int8_t *)"usage: cat <path>\n");
        return 1;
    }

    fd = u_open((const int8_t *)argv[1], STUPIDOS_O_RDONLY);
    if (fd < 0)
    {
        u_puts((const int8_t *)"cat: open failed\n");
        return 1;
    }

    while (1)
    {
        nread = u_read(fd, buf, sizeof(buf));
        if (nread < 0)
        {
            u_puts((const int8_t *)"cat: read failed\n");
            u_close(fd);
            return 1;
        }

        if (nread == 0)
        {
            break;
        }

        (void)u_write(STUPIDOS_STDOUT_FILENO, buf, (size_t)nread);
    }

    u_close(fd);
    return 0;
}
