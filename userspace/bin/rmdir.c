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
    puts_stderr("rmdir: ");
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
    int i;
    bool ok;

    if (argc < 2)
    {
        puts_stderr("usage: rmdir <path>...\n");
        return 1;
    }

    ok = true;
    for (i = 1; i < argc; i++)
    {
        if (!argv[i] || argv[i][0] == '\0')
        {
            continue;
        }

        if (rmdir(argv[i]) < 0)
        {
            print_err(argv[i]);
            ok = false;
        }
    }

    return ok ? 0 : 1;
}
