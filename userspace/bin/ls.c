#include "stupidos_user.h"

int main(int argc, char **argv)
{
    struct stupidos_dirent ent;
    const int8_t *path;
    int ret;
    uint32_t index;

    path = (argc > 1 && argv[1]) ? (const int8_t *)argv[1] : (const int8_t *)"/";
    index = 0;
    while (1)
    {
        ret = u_readdir(path, index, &ent);
        if (ret == -STUPIDOS_ENOENT)
        {
            break;
        }
        if (ret < 0)
        {
            u_puts((const int8_t *)"ls: readdir failed\n");
            return 1;
        }

        u_puts((ent.mode & STUPIDOS_VFS_S_IFDIR) ? (const int8_t *)"[d] " : (const int8_t *)"[-] ");
        u_puts(ent.name);
        u_puts((const int8_t *)"\n");
        index++;
    }

    return 0;
}
