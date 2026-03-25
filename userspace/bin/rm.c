#include "stupidos_user.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>

static void puts_stderr(const char *s)
{
    if (!s)
    {
        return;
    }
    (void)u_write(STUPIDOS_STDERR_FILENO, s, strlen(s));
}

static void print_err(const char *path)
{
    puts_stderr("rm: ");
    if (path)
    {
        puts_stderr(path);
        puts_stderr(": ");
    }
    puts_stderr(strerror(errno));
    puts_stderr("\n");
}

int main(int argc, char **argv)
{
    bool opt_f;
    bool opt_r;
    int i;
    bool ok;
    int ret;

    opt_f = false;
    opt_r = false;
    i = 1;
    while (i < argc && argv[i] && argv[i][0] == '-')
    {
        if (strcmp(argv[i], "-f") == 0)
        {
            opt_f = true;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "-R") == 0)
        {
            opt_r = true;
            i++;
            continue;
        }
        if (strcmp(argv[i], "--") == 0)
        {
            i++;
            break;
        }

        puts_stderr("usage: rm [-f] [-r] <path>...\n");
        return 1;
    }

    if (i >= argc)
    {
        puts_stderr("usage: rm [-f] [-r] <path>...\n");
        return 1;
    }

    ok = true;
    for (; i < argc; i++)
    {
        if (!argv[i] || argv[i][0] == '\0')
        {
            continue;
        }

        ret = unlink(argv[i]);
        if (ret == 0)
        {
            continue;
        }

        if (opt_r)
        {
            ret = rmdir(argv[i]);
            if (ret == 0)
            {
                continue;
            }
        }

        if (opt_f && (errno == ENOENT))
        {
            continue;
        }

        print_err(argv[i]);
        ok = false;
    }

    return ok ? 0 : 1;
}
