#include "stupidos_user.h"
#include "errno.h"

/*
 * stupidos 的 BusyBox 风格入口。
 *
 * 设计目标很实用：
 * - 先提供一个统一入口 `busybox`；
 * - 其中 `wget` 直接走 BusyBox 派生实现，其他 applet 先回退到现有独立 ELF；
 * - 这样用户马上就能体验到 BusyBox 的“单一入口、多命令”的使用方式，
 *   同时不会打断现有的系统可用性。
 */

extern int busybox_wget_main(int argc, char **argv);

static const int8_t *const busybox_known_applets[] =
{
    (const int8_t *)"wget",
    (const int8_t *)"ftp",
    (const int8_t *)"ftpget",
    (const int8_t *)"ftpput",
    (const int8_t *)"echo",
    (const int8_t *)"cat",
    (const int8_t *)"ls",
    (const int8_t *)"mkdir",
    (const int8_t *)"rmdir",
    (const int8_t *)"rm",
    (const int8_t *)"mv",
    (const int8_t *)"touch",
    (const int8_t *)"ping",
    (const int8_t *)"sleep",
    (const int8_t *)"netcfg",
    (const int8_t *)"sh",
    (const int8_t *)"tcc",
    (const int8_t *)"mkprobe",
    (const int8_t *)"elfinfo",
    (const int8_t *)"hello",
    (const int8_t *)"python3",
    (const int8_t *)"vi",
    (const int8_t *)"vim",
};

static const int8_t *busybox_basename(const int8_t *path)
{
    size_t i;
    const int8_t *base;

    if (!path || path[0] == '\0')
    {
        return (const int8_t *)"busybox";
    }

    base = path;
    for (i = 0; path[i] != '\0'; i++)
    {
        if (path[i] == '/')
        {
            base = &path[i + 1];
        }
    }
    if (base[0] == '\0')
    {
        return (const int8_t *)"busybox";
    }
    return base;
}

static int busybox_is_known_applet(const int8_t *name)
{
    size_t i;

    if (!name || name[0] == '\0')
    {
        return 0;
    }

    for (i = 0; i < sizeof(busybox_known_applets) / sizeof(busybox_known_applets[0]); i++)
    {
        if (u_strcmp(name, busybox_known_applets[i]) == 0)
        {
            return 1;
        }
    }

    return 0;
}

static int busybox_exec_external(const int8_t *applet, int argc, char **argv)
{
    static const int8_t *const prefixes[] =
    {
        (const int8_t *)"/bin/",
        (const int8_t *)"/usr/bin/",
        (const int8_t *)"/usr/local/bin/",
    };
    int8_t path[STUPIDOS_PATH_MAX];
    int pid;
    size_t i;
    size_t prefix_len;
    size_t applet_len;

    if (!applet || applet[0] == '\0')
    {
        return -EINVAL;
    }

    applet_len = u_strnlen(applet, sizeof(path) - 1U);
    for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
    {
        prefix_len = u_strnlen(prefixes[i], sizeof(path) - 1U);
        if (prefix_len + applet_len + 1U >= sizeof(path))
        {
            continue;
        }

        u_memset(path, 0, sizeof(path));
        u_memcpy(path, prefixes[i], prefix_len);
        u_memcpy(path + prefix_len, applet, applet_len);
        path[prefix_len + applet_len] = '\0';

        pid = u_exec(path, argc, (const int8_t **)argv);
        if (pid >= 0)
        {
            (void)u_waitpid((int32_t)pid);
            return 0;
        }

        if (pid != -ENOENT)
        {
            return pid;
        }
    }

    return -ENOENT;
}

static void busybox_usage(void)
{
    u_puts((const int8_t *)
           "usage: busybox <applet> [args...]\n"
           "       busybox --list\n"
           "\n"
           "built-in applets:\n");
    for (size_t i = 0; i < sizeof(busybox_known_applets) / sizeof(busybox_known_applets[0]); i++)
    {
        u_puts((const int8_t *)"  ");
        u_puts(busybox_known_applets[i]);
        u_puts((const int8_t *)"\n");
    }
}

int main(int argc, char **argv)
{
    const int8_t *applet;
    int rc;

    applet = busybox_basename((const int8_t *)argv[0]);
    if (u_strcmp(applet, (const int8_t *)"busybox") != 0)
    {
        if (u_strcmp(applet, (const int8_t *)"wget") == 0)
        {
            return busybox_wget_main(argc, argv);
        }
        if (!busybox_is_known_applet(applet))
        {
            busybox_usage();
            return 1;
        }
        rc = busybox_exec_external(applet, argc, argv);
        if (rc == -ENOENT)
        {
            busybox_usage();
            return 1;
        }
        return rc == 0 ? 0 : 1;
    }

    if (argc < 2 || !argv[1])
    {
        busybox_usage();
        return 1;
    }

    if (u_strcmp((const int8_t *)argv[1], (const int8_t *)"--list") == 0)
    {
        busybox_usage();
        return 0;
    }

    if (u_strcmp((const int8_t *)argv[1], (const int8_t *)"wget") == 0)
    {
        return busybox_wget_main(argc - 1, &argv[1]);
    }

    if (!busybox_is_known_applet((const int8_t *)argv[1]))
    {
        busybox_usage();
        return 1;
    }

    rc = busybox_exec_external((const int8_t *)argv[1], argc - 1, &argv[1]);
    if (rc == -ENOENT)
    {
        busybox_usage();
        return 1;
    }
    return rc == 0 ? 0 : 1;
}
