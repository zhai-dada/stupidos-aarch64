#include "stupidos_user.h"
#include <sys/stat.h>
#include <unistd.h>

#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

static int8_t ls_mode_type_char(uint32_t mode)
{
    switch (mode & STUPIDOS_VFS_S_IFMT)
    {
    case STUPIDOS_VFS_S_IFDIR:
        return 'd';
    case STUPIDOS_VFS_S_IFCHR:
        return 'c';
    case STUPIDOS_VFS_S_IFLNK:
        return 'l';
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
    int8_t link_target[STUPIDOS_PATH_MAX];
    ssize_t link_len;

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
    if ((st->mode & STUPIDOS_VFS_S_IFMT) == STUPIDOS_VFS_S_IFLNK)
    {
        u_memset(link_target, 0, sizeof(link_target));
        link_len = u_readlink(path, link_target, sizeof(link_target) - 1U);
        if (link_len >= 0)
        {
            link_target[(size_t)link_len] = '\0';
            u_puts((const int8_t *)" -> ");
            u_puts(link_target);
        }
    }
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

static int ls_list_dir(const int8_t *path)
{
    struct stupidos_dirent ent;
    struct stupidos_stat st;
    struct stat lst;
    int8_t full_path[STUPIDOS_PATH_MAX * 2];
    int ret;
    uint32_t index;

    index = 0;
    while (1)
    {
        ret = u_readdir(path, index, &ent);
        if (ret == -STUPIDOS_ENOENT)
        {
            return 0;
        }
        if (ret < 0)
        {
            u_puts((const int8_t *)"ls: readdir failed\n");
            return 1;
        }

        ls_join_path(full_path, sizeof(full_path), path, ent.name);
        if (lstat((const char *)full_path, &lst) < 0)
        {
            u_memset(&st, 0, sizeof(st));
            st.mode = ent.mode;
            st.size = ent.size;
            st.nlink = 1;
            st.uid = 0;
            st.gid = 0;
            st.blksize = 4096;
        }
        else
        {
            st.mode = (uint32_t)lst.st_mode;
            st.nlink = (uint32_t)lst.st_nlink;
            st.uid = (uint32_t)lst.st_uid;
            st.gid = (uint32_t)lst.st_gid;
            st.size = (uint64_t)lst.st_size;
            st.blksize = (uint32_t)lst.st_blksize;
        }

        ls_print_long(full_path, ent.name, &st);
        index++;
    }
}

/*
 * 兼容 Linux 常见 ls 行为：
 * - 目标是普通文件时直接打印文件信息
 * - 目标是目录时列出目录内容
 */
static int ls_list_target(const int8_t *path)
{
    struct stupidos_stat st;

    if (!path || path[0] == '\0')
    {
        path = (const int8_t *)".";
    }

    if (u_stat(path, &st) < 0)
    {
        u_puts((const int8_t *)"ls: cannot access ");
        u_puts(path);
        u_puts((const int8_t *)"\n");
        return 1;
    }

    if ((st.mode & STUPIDOS_VFS_S_IFMT) != STUPIDOS_VFS_S_IFDIR)
    {
        ls_print_long(path, path, &st);
        return 0;
    }

    return ls_list_dir(path);
}

int main(int argc, char **argv)
{
    int8_t cwd_buf[STUPIDOS_PATH_MAX];
    int rc;
    int i;

    rc = 0;
    if (argc <= 1)
    {
        u_memset(cwd_buf, 0, sizeof(cwd_buf));
        if (u_getcwd(cwd_buf, sizeof(cwd_buf)) < 0 || cwd_buf[0] == '\0')
        {
            return ls_list_target((const int8_t *)"/");
        }
        return ls_list_target(cwd_buf);
    }

    for (i = 1; i < argc; i++)
    {
        if (!argv[i] || argv[i][0] == '\0')
        {
            continue;
        }

        if (argc > 2)
        {
            u_puts((const int8_t *)argv[i]);
            u_puts((const int8_t *)":\n");
        }

        if (ls_list_target((const int8_t *)argv[i]) != 0)
        {
            rc = 1;
        }

        if (argc > 2 && i + 1 < argc)
        {
            u_puts((const int8_t *)"\n");
        }
    }

    return rc;
}
