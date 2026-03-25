#include "stupidos_user.h"

static size_t zlen(const char *s)
{
    size_t n = 0;

    while (s && s[n] != '\0')
    {
        n++;
    }
    return n;
}

static void put(const char *s)
{
    if (!s)
    {
        return;
    }
    (void)u_write(STUPIDOS_STDOUT_FILENO, s, zlen(s));
}

int main(int argc, char **argv)
{
    int64_t ret;
    const int8_t *path;

    if (argc < 2 || !argv[1] || argv[1][0] == '\0')
    {
        put("usage: mkprobe <path>\n");
        return 1;
    }

    path = (const int8_t *)argv[1];
    put("mkprobe:path=");
    put((const char *)path);
    put("\n");
    ret = u_mkdir(path, 0755);
    if (ret < 0)
    {
        put("mkprobe: mkdir failed\n");
        return 2;
    }

    put("mkprobe: mkdir ok\n");
    return 0;
}
