#include "stupidos_user.h"

#define EI_NIDENT 16
#define PT_LOAD   1U

struct elf64_ehdr
{
    uint8_t e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf64_phdr
{
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

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

static void put_u64_dec(uint64_t v)
{
    char buf[32];
    int i = 0;

    if (v == 0)
    {
        put("0");
        return;
    }

    while (v && i < (int)sizeof(buf))
    {
        buf[i++] = (char)('0' + (v % 10U));
        v /= 10U;
    }

    while (i > 0)
    {
        char ch = buf[--i];
        (void)u_write(STUPIDOS_STDOUT_FILENO, &ch, 1);
    }
}

static void put_u64_hex(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    char out[18];
    int i;

    out[0] = '0';
    out[1] = 'x';
    for (i = 0; i < 16; i++)
    {
        out[17 - i] = hex[(int)(v & 0xFU)];
        v >>= 4U;
    }
    (void)u_write(STUPIDOS_STDOUT_FILENO, out, sizeof(out));
}

static int elfinfo_one(const char *path)
{
    int fd;
    struct elf64_ehdr eh;
    uint16_t i;

    put("file: ");
    put(path);
    put("\n");

    fd = u_open((const int8_t *)path, STUPIDOS_O_RDONLY);
    if (fd < 0)
    {
        put("  open: failed\n");
        return 1;
    }

    if (u_read(fd, &eh, sizeof(eh)) != (ssize_t)sizeof(eh))
    {
        put("  read ehdr: failed\n");
        (void)u_close(fd);
        return 2;
    }

    if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L' || eh.e_ident[3] != 'F')
    {
        put("  not an ELF file\n");
        (void)u_close(fd);
        return 3;
    }

    put("  e_type=");
    put_u64_dec(eh.e_type);
    put(" e_machine=");
    put_u64_dec(eh.e_machine);
    put(" e_entry=");
    put_u64_hex(eh.e_entry);
    put("\n");

    put("  e_phoff=");
    put_u64_hex(eh.e_phoff);
    put(" e_phentsize=");
    put_u64_dec(eh.e_phentsize);
    put(" e_phnum=");
    put_u64_dec(eh.e_phnum);
    put("\n");

    if (!eh.e_phoff || !eh.e_phnum || eh.e_phentsize < sizeof(struct elf64_phdr))
    {
        put("  no valid program headers\n");
        (void)u_close(fd);
        return 0;
    }

    if (u_lseek(fd, (int64_t)eh.e_phoff, STUPIDOS_SEEK_SET) < 0)
    {
        put("  seek phdr: failed\n");
        (void)u_close(fd);
        return 4;
    }

    for (i = 0; i < eh.e_phnum; i++)
    {
        struct elf64_phdr ph;

        if (u_read(fd, &ph, sizeof(ph)) != (ssize_t)sizeof(ph))
        {
            put("  read phdr: failed\n");
            (void)u_close(fd);
            return 5;
        }

        if (ph.p_type != PT_LOAD)
        {
            continue;
        }

        put("  LOAD ");
        put_u64_dec((uint64_t)i);
        put(": off=");
        put_u64_hex(ph.p_offset);
        put(" vaddr=");
        put_u64_hex(ph.p_vaddr);
        put(" filesz=");
        put_u64_hex(ph.p_filesz);
        put(" memsz=");
        put_u64_hex(ph.p_memsz);
        put(" flags=");
        put_u64_hex(ph.p_flags);
        put("\n");
    }

    (void)u_close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    int i;
    int rc = 0;

    if (argc < 2)
    {
        put("usage: elfinfo <elf-path> [elf-path...]\n");
        return 1;
    }

    for (i = 1; i < argc; i++)
    {
        if (!argv[i] || argv[i][0] == '\0')
        {
            continue;
        }
        if (elfinfo_one(argv[i]) != 0)
        {
            rc = 2;
        }
    }

    return rc;
}
