#include "stupidos_user.h"

static int8_t ls_mode_type_char(uint32_t mode)
{
    switch (mode & STUPIDOS_VFS_S_IFMT)
    {
    case STUPIDOS_VFS_S_IFDIR:
        return 'd';
    case STUPIDOS_VFS_S_IFCHR:
        return 'c';
    case STUPIDOS_VFS_S_IFREG:
        return '-';
    default:
        return '?';
    }
}

static void ls_mode_perm_string(uint32_t mode, int8_t out[11])
{
    out[0] = ls_mode_type_char(mode);
    out[1] = (mode & STUPIDOS_VFS_S_IRUSR) ? 'r' : '-';
    out[2] = (mode & STUPIDOS_VFS_S_IWUSR) ? 'w' : '-';
    out[3] = (mode & STUPIDOS_VFS_S_IXUSR) ? 'x' : '-';
    out[4] = (mode & STUPIDOS_VFS_S_IRGRP) ? 'r' : '-';
    out[5] = (mode & STUPIDOS_VFS_S_IWGRP) ? 'w' : '-';
    out[6] = (mode & STUPIDOS_VFS_S_IXGRP) ? 'x' : '-';
    out[7] = (mode & STUPIDOS_VFS_S_IROTH) ? 'r' : '-';
    out[8] = (mode & STUPIDOS_VFS_S_IWOTH) ? 'w' : '-';
    out[9] = (mode & STUPIDOS_VFS_S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static void ls_print_u64(uint64_t value)
{
    int8_t buf[32];
    size_t pos;

    pos = sizeof(buf);
    buf[--pos] = '\0';
    if (value == 0)
    {
        buf[--pos] = '0';
        u_puts(&buf[pos]);
        return;
    }

    while (value > 0 && pos > 0)
    {
        buf[--pos] = (int8_t)('0' + (value % 10ULL));
        value /= 10ULL;
    }

    u_puts(&buf[pos]);
}

static void ls_print_long(const int8_t *path, const int8_t *name, const struct stupidos_stat *st)
{
    int8_t perm[11];

    ls_mode_perm_string((uint32_t)st->mode, perm);
    u_puts(perm);
    u_puts((const int8_t *)" ");
    ls_print_u64((uint64_t)st->nlink);
    u_puts((const int8_t *)" ");
    ls_print_u64((uint64_t)st->uid);
    u_puts((const int8_t *)" ");
    ls_print_u64((uint64_t)st->gid);
    u_puts((const int8_t *)" ");
    ls_print_u64(st->size);
    u_puts((const int8_t *)" ");
    u_puts(name);
    (void)path;
    u_puts((const int8_t *)"\n");
}

static void ls_join_path(int8_t *out, size_t out_len, const int8_t *base, const int8_t *name)
{
    size_t i;
    size_t pos;

    if (!out || out_len == 0)
    {
        return;
    }

    out[0] = '\0';
    pos = 0;
    if (base && base[0] != '\0')
    {
        for (i = 0; base[i] != '\0' && pos + 1 < out_len; i++)
        {
            out[pos++] = base[i];
        }
    }

    if (pos == 0)
    {
        if (pos + 1 < out_len)
        {
            out[pos++] = '/';
        }
    }
    else if (out[pos - 1] != '/' && pos + 1 < out_len)
    {
        out[pos++] = '/';
    }

    if (name)
    {
        for (i = 0; name[i] != '\0' && pos + 1 < out_len; i++)
        {
            out[pos++] = name[i];
        }
    }

    out[pos] = '\0';
}

int main(int argc, char **argv)
{
    struct stupidos_dirent ent;
    struct stupidos_stat st;
    int8_t full_path[STUPIDOS_PATH_MAX * 2];
    int8_t cwd_buf[STUPIDOS_PATH_MAX];
    const int8_t *path;
    int ret;
    uint32_t index;

    if (argc > 1 && argv[1] && argv[1][0] != '\0')
    {
        path = (const int8_t *)argv[1];
    }
    else
    {
        u_memset(cwd_buf, 0, sizeof(cwd_buf));
        if (u_getcwd(cwd_buf, sizeof(cwd_buf)) < 0 || cwd_buf[0] == '\0')
        {
            path = (const int8_t *)"/";
        }
        else
        {
            path = cwd_buf;
        }
    }

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

        ls_join_path(full_path, sizeof(full_path), path, ent.name);
        if (u_stat(full_path, &st) < 0)
        {
            u_memset(&st, 0, sizeof(st));
            st.mode = ent.mode;
            st.size = ent.size;
            st.nlink = 1;
            st.uid = 0;
            st.gid = 0;
            st.blksize = 4096;
        }

        ls_print_long(path, ent.name, &st);
        index++;
    }

    return 0;
}
