#include "stupidos_user.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void puts_stderr(const char *s)
{
    if (!s)
    {
        return;
    }
    (void)u_write(STUPIDOS_STDERR_FILENO, s, strlen(s));
}

int main(int argc, char **argv)
{
    if (argc != 3 || !argv[1] || !argv[2])
    {
        puts_stderr("usage: mv <source> <target>\n");
        return 1;
    }

    if (rename(argv[1], argv[2]) < 0)
    {
        puts_stderr("mv: ");
        puts_stderr(argv[1]);
        puts_stderr(" -> ");
        puts_stderr(argv[2]);
        puts_stderr(": ");
        puts_stderr(strerror(errno));
        puts_stderr("\n");
        return 1;
    }

    return 0;
}
