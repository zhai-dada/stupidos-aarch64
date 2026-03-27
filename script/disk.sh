set -eu

DISK_IMG="disk.img"
SECTOR_SIZE=512
P1_START=2048
P1_SECTORS=2097152
P2_START=2099200
P2_SECTORS=4192223
EXT4_BLOCK_SIZE=4096
EXT4_BLOCKS=$((P2_SECTORS * SECTOR_SIZE / EXT4_BLOCK_SIZE))
P2_OFFSET=$((P2_START * SECTOR_SIZE))
TMP_ROOT="$(mktemp -d)"
TMP_FAT="$(mktemp -d)"
P1_OFFSET=$((P1_START * SECTOR_SIZE))
PYTHON_BIN="${PYTHON_BIN:-}"
PYTHON_LIB_DIR="${PYTHON_LIB_DIR:-}"
TCC_SRC_DIR="${TCC_SRC_DIR:-}"
TCC_LIBTCC1="${TCC_LIBTCC1:-}"
USER_BINS_LIST="${USER_BINS_LIST:-}"
USER_INC_DIR="${USER_INC_DIR:-}"
USER_RUNTIME_LIB="${USER_RUNTIME_LIB:-}"
USER_CRT0_OBJ="${USER_CRT0_OBJ:-}"
USER_LIBGCC="${USER_LIBGCC:-}"

cleanup() {
    rm -rf "$TMP_ROOT"
    rm -rf "$TMP_FAT"
}

trap cleanup EXIT

dd if=/dev/zero of="$DISK_IMG" bs=512M count=6 status=none
cat fdisk.args | fdisk "$DISK_IMG" >/dev/null

mkdir -p "$TMP_ROOT/etc" "$TMP_ROOT/boot" "$TMP_ROOT/bin" "$TMP_ROOT/usr/bin" "$TMP_ROOT/usr/share/examples"
printf "hello from ext4 root\n" > "$TMP_ROOT/hello.txt"
printf "rewrite me via kernel vfs\n" > "$TMP_ROOT/rw-demo.txt"
printf "nested file\n" > "$TMP_ROOT/etc/info.txt"
printf "mount point for fat32\n" > "$TMP_ROOT/boot/README"
cat > "$TMP_ROOT/usr/share/examples/min.c" <<'EOF'
/*
 * tinycc smoke test input:
 * 只依赖 C 语法本身，避免把“编译器问题”和“libc/链接问题”混在一起。
 */
int add(int a, int b)
{
    return a + b;
}
EOF
cat > "$TMP_ROOT/usr/share/examples/main0.c" <<'EOF'
/*
 * tinycc link smoke test input:
 * 不依赖标准库，方便验证 tcc + 自身链接流程。
 */
int main(void)
{
    return 0;
}
EOF
cat > "$TMP_ROOT/usr/share/examples/hello_stdio.c" <<'EOF'
#include <stdio.h>

int main(void)
{
    puts("hello from tcc+libc");
    return 0;
}
EOF
cat > "$TMP_ROOT/usr/share/examples/hello_uputs.c" <<'EOF'
#include "stupidos_user.h"

int main(void)
{
    u_puts((const int8_t *)"hello from tcc+u_puts\n");
    return 0;
}
EOF
cat > "$TMP_ROOT/usr/share/examples/hello_write.c" <<'EOF'
#include <fcntl.h>
#include <unistd.h>

int main(void)
{
    static const char msg[] = "hello from tcc+write\n";
    int fd;

    fd = open("/tmp/tcc_probe.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
    {
        (void)write(fd, msg, sizeof(msg) - 1U);
        (void)close(fd);
    }

    (void)write(1, msg, sizeof(msg) - 1U);
    return 0;
}
EOF

if [ -n "$USER_BINS_LIST" ]; then
    for bin in $USER_BINS_LIST; do
        if [ -f "$bin" ]; then
            cp "$bin" "$TMP_ROOT/bin/$(basename "$bin")"
        fi
    done
elif [ -n "${USER_BINS_DIR:-}" ] && [ -d "$USER_BINS_DIR" ]; then
    find "$USER_BINS_DIR" -maxdepth 1 -type f -perm -u+x -exec cp {} "$TMP_ROOT/bin/" \;
fi

if [ -d "$TMP_ROOT/bin" ]; then
    find "$TMP_ROOT/bin" -maxdepth 1 -type f -exec chmod 755 {} \;
    # 兼容常见 Linux 布局：把用户程序同步到 /usr/bin，便于脚本按标准路径调用。
    find "$TMP_ROOT/bin" -maxdepth 1 -type f -exec cp {} "$TMP_ROOT/usr/bin/" \;
    find "$TMP_ROOT/usr/bin" -maxdepth 1 -type f -exec chmod 755 {} \;
    if [ -f "$TMP_ROOT/bin/python3" ] && [ ! -f "$TMP_ROOT/bin/python" ]; then
        cp "$TMP_ROOT/bin/python3" "$TMP_ROOT/bin/python"
        chmod 755 "$TMP_ROOT/bin/python"
    fi
    if [ -f "$TMP_ROOT/bin/ftp" ]; then
        cp "$TMP_ROOT/bin/ftp" "$TMP_ROOT/bin/ftpget"
        cp "$TMP_ROOT/bin/ftp" "$TMP_ROOT/bin/ftpput"
        chmod 755 "$TMP_ROOT/bin/ftpget" "$TMP_ROOT/bin/ftpput"
        cp "$TMP_ROOT/bin/ftpget" "$TMP_ROOT/usr/bin/ftpget"
        cp "$TMP_ROOT/bin/ftpput" "$TMP_ROOT/usr/bin/ftpput"
        chmod 755 "$TMP_ROOT/usr/bin/ftpget" "$TMP_ROOT/usr/bin/ftpput"
    fi
fi

if [ -n "$PYTHON_BIN" ] && [ -f "$PYTHON_BIN" ]; then
    # 先把 Python 作为真正的目标系统可执行文件装进 rootfs。
    # 这里同时放到 /bin 和 /usr/local/bin，兼容 shell 的 PATH 搜索和未来的标准布局。
    mkdir -p "$TMP_ROOT/bin" \
        "$TMP_ROOT/usr/local/bin" \
        "$TMP_ROOT/usr/local/lib/python3.10"
    cp "$PYTHON_BIN" "$TMP_ROOT/bin/python"
    cp "$PYTHON_BIN" "$TMP_ROOT/bin/python3"
    cp "$PYTHON_BIN" "$TMP_ROOT/usr/local/bin/python3.10"
    cp "$PYTHON_BIN" "$TMP_ROOT/usr/local/bin/python3"
    cp "$PYTHON_BIN" "$TMP_ROOT/usr/local/bin/python"
    chmod 755 "$TMP_ROOT/bin/python" \
        "$TMP_ROOT/bin/python3" \
        "$TMP_ROOT/usr/local/bin/python3.10" \
        "$TMP_ROOT/usr/local/bin/python3" \
        "$TMP_ROOT/usr/local/bin/python"
fi

if [ -n "$PYTHON_LIB_DIR" ] && [ -d "$PYTHON_LIB_DIR" ]; then
    # 标准库目录直接复制到 /usr/local/lib/python3.10。
    # Python 启动和最基本的 import 需要这里的纯 Python 模块。
    mkdir -p "$TMP_ROOT/usr/local/lib/python3.10" "$TMP_ROOT/usr/local/lib/lib-dynload"
    cp -a "$PYTHON_LIB_DIR"/. "$TMP_ROOT/usr/local/lib/python3.10/"
    find "$TMP_ROOT/usr/local/lib/python3.10" -type d -name "__pycache__" -prune -exec rm -rf {} +
    rm -rf "$TMP_ROOT/usr/local/lib/python3.10/test" \
        "$TMP_ROOT/usr/local/lib/python3.10/idlelib" \
        "$TMP_ROOT/usr/local/lib/python3.10/tkinter" \
        "$TMP_ROOT/usr/local/lib/python3.10/lib2to3" 2>/dev/null || true
fi

if [ -n "$TCC_SRC_DIR" ] && [ -d "$TCC_SRC_DIR/include" ]; then
    # 为 tinycc 提供运行时私有头文件目录，修复 guest 内 `tccdefs.h` 缺失。
    mkdir -p "$TMP_ROOT/usr/local/lib/tcc"
    cp -a "$TCC_SRC_DIR/include" "$TMP_ROOT/usr/local/lib/tcc/"
fi

if [ -n "$TCC_LIBTCC1" ] && [ -f "$TCC_LIBTCC1" ]; then
    # tinycc 运行时库是 guest 内编译与链接的重要依赖。
    # 这里安装 ARM64 版本的 libtcc1.a，避免 tcc 在 link 阶段找不到自身 helper。
    mkdir -p "$TMP_ROOT/usr/local/lib/tcc"
    cp "$TCC_LIBTCC1" "$TMP_ROOT/usr/local/lib/tcc/$(basename "$TCC_LIBTCC1")"
    cp "$TCC_LIBTCC1" "$TMP_ROOT/usr/local/lib/tcc/libtcc1.a"
fi

if [ -n "$USER_INC_DIR" ] && [ -d "$USER_INC_DIR" ]; then
    # 安装系统头文件，给后续 tcc/python/tcc 移植留统一入口。
    mkdir -p "$TMP_ROOT/usr/include"
    cp -a "$USER_INC_DIR"/. "$TMP_ROOT/usr/include/"
fi

if [ -n "$USER_RUNTIME_LIB" ] && [ -f "$USER_RUNTIME_LIB" ]; then
    # 安装最小用户态运行库，支持 guest 内 tcc 直接链接出可执行 ELF。
    mkdir -p "$TMP_ROOT/usr/lib"
    cp "$USER_RUNTIME_LIB" "$TMP_ROOT/usr/lib/libstupidos.a"
fi

if [ -n "$USER_CRT0_OBJ" ] && [ -f "$USER_CRT0_OBJ" ]; then
    mkdir -p "$TMP_ROOT/usr/lib"
    cp "$USER_CRT0_OBJ" "$TMP_ROOT/usr/lib/crt0.o"
fi

if [ -n "$USER_LIBGCC" ] && [ -f "$USER_LIBGCC" ]; then
    mkdir -p "$TMP_ROOT/usr/lib"
    cp "$USER_LIBGCC" "$TMP_ROOT/usr/lib/libgcc.a"
fi

printf "hello from fat32 boot volume\n" > "$TMP_FAT/README.TXT"
printf "kernel overwrites me via fat32\n" > "$TMP_FAT/RWDEMO.TXT"
printf "fat32 nested file\n" > "$TMP_FAT/INFO.TXT"

mkfs.fat -F 32 -s 8 --offset="$P1_START" "$DISK_IMG" $((P1_SECTORS / 2)) >/dev/null 2>&1
mmd -i "$DISK_IMG@@$P1_OFFSET" ::/EFI >/dev/null
mmd -i "$DISK_IMG@@$P1_OFFSET" ::/EFI/BOOT >/dev/null
mcopy -i "$DISK_IMG@@$P1_OFFSET" "$TMP_FAT/README.TXT" ::/README.TXT >/dev/null
mcopy -i "$DISK_IMG@@$P1_OFFSET" "$TMP_FAT/RWDEMO.TXT" ::/RWDEMO.TXT >/dev/null
mcopy -i "$DISK_IMG@@$P1_OFFSET" "$TMP_FAT/INFO.TXT" ::/EFI/BOOT/INFO.TXT >/dev/null
#
# 当前内核里的 ext4 目录遍历只实现了最小线性目录格式，
# 还没有支持 htree(dir_index) 目录索引。
# 如果 mkfs 默认把 /bin 之类目录做成 indexed directory，
# lookup("/bin/ls") 可能还能靠顺扫命中，但 readdir("/bin") 会看起来像空目录。
# 这里显式关闭 dir_index，让磁盘镜像和当前 ext4 驱动能力保持一致。
#
mkfs.ext4 -F -q -O ^dir_index -b "$EXT4_BLOCK_SIZE" -d "$TMP_ROOT" \
    -E "offset=$P2_OFFSET" "$DISK_IMG" "$EXT4_BLOCKS"
