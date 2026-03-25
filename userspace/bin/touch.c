#include "stupidos_user.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
    puts_stderr("touch: ");
    if (path)
    {
        puts_stderr(path);
        puts_stderr(": ");
    }
    puts_stderr(strerror(errno));
    puts_stderr("\n");
}

static int touch_one(const char *path)
{
    int fd;

    if (!path || path[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }

    /* 先尝试更新时间戳，不存在再尝试创建。 */
    if (utimensat(AT_FDCWD, path, 0, 0) == 0)
    {
        return 0;
    }

    if (errno != ENOENT)
    {
        return -1;
    }

    fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0)
    {
        return -1;
    }

    (void)close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    int i;
    bool ok;

    if (argc < 2)
    {
        puts_stderr("usage: touch <file>...\n");
        return 1;
    }

    ok = true;
    for (i = 1; i < argc; i++)
    {
        if (!argv[i] || argv[i][0] == '\0')
        {
            continue;
        }

        if (touch_one(argv[i]) < 0)
        {
            print_err(argv[i]);
            ok = false;
        }
    }

    return ok ? 0 : 1;
}
