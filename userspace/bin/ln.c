#include "stupidos_user.h"
#include <unistd.h>
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
    int i;
    int use_symlink;
    const char *src;
    const char *dst;

    use_symlink = 0;
    src = 0;
    dst = 0;

    if (argc == 4 && argv[1] && strcmp(argv[1], "-s") == 0)
    {
        use_symlink = 1;
        src = argv[2];
        dst = argv[3];
    }
    else if (argc == 3)
    {
        src = argv[1];
        dst = argv[2];
    }
    else
    {
        puts_stderr("usage: ln [-s] <source> <target>\n");
        return 1;
    }

    for (i = 1; i < argc; i++)
    {
        if (!argv[i])
        {
            puts_stderr("ln: invalid argument\n");
            return 1;
        }
    }

    if (use_symlink)
    {
        if (symlink(src, dst) < 0)
        {
            puts_stderr("ln: ");
            puts_stderr(src);
            puts_stderr(" -> ");
            puts_stderr(dst);
            puts_stderr(": ");
            puts_stderr(strerror(errno));
            puts_stderr("\n");
            return 1;
        }
        return 0;
    }

    if (link(src, dst) < 0)
    {
        puts_stderr("ln: ");
        puts_stderr(src);
        puts_stderr(" -> ");
        puts_stderr(dst);
        puts_stderr(": ");
        puts_stderr(strerror(errno));
        puts_stderr("\n");
        return 1;
    }

    return 0;
}
