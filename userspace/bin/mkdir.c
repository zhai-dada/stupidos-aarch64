#include "stupidos_user.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

static void puts_stderr(const char *s)
{
    if (!s)
    {
        return;
    }
    (void)u_write(STUPIDOS_STDERR_FILENO, s, strlen(s));
}

static void print_err(const char *op, const char *path)
{
    puts_stderr("mkdir: ");
    if (op)
    {
        puts_stderr(op);
        puts_stderr(": ");
    }
    if (path)
    {
        puts_stderr(path);
        puts_stderr(": ");
    }
    puts_stderr(strerror(errno));
    puts_stderr("\n");
}

static int mkdir_p(const char *path, mode_t mode)
{
    char buf[STUPIDOS_PATH_MAX];
    size_t i;
    size_t n;

    if (!path || path[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }

    n = strlen(path);
    if (n >= sizeof(buf))
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(buf, path, n + 1U);
    for (i = 1; i < n; i++)
    {
        if (buf[i] != '/')
        {
            continue;
        }

        buf[i] = '\0';
        if (buf[0] != '\0' && mkdir(buf, mode) < 0 && errno != EEXIST)
        {
            return -1;
        }
        buf[i] = '/';
    }

    if (mkdir(buf, mode) < 0 && errno != EEXIST)
    {
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    bool opt_p;
    int i;
    mode_t mode;
    int ret;
    bool ok;

    opt_p = false;
    mode = 0755;
    i = 1;
    while (i < argc && argv[i] && argv[i][0] == '-')
    {
        if (strcmp(argv[i], "-p") == 0)
        {
            opt_p = true;
            i++;
            continue;
        }

        if (strcmp(argv[i], "--") == 0)
        {
            i++;
            break;
        }

        puts_stderr("usage: mkdir [-p] <path>...\n");
        return 1;
    }

    if (i >= argc)
    {
        puts_stderr("usage: mkdir [-p] <path>...\n");
        return 1;
    }

    ok = true;
    for (; i < argc; i++)
    {
        if (!argv[i] || argv[i][0] == '\0')
        {
            continue;
        }

        if (opt_p)
        {
            ret = mkdir_p(argv[i], mode);
        }
        else
        {
            ret = mkdir(argv[i], mode);
        }

        if (ret < 0)
        {
            print_err("create", argv[i]);
            ok = false;
        }
    }

    if (!ok)
    {
        return 1;
    }

    return 0;
}
