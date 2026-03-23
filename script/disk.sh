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

cleanup() {
    rm -rf "$TMP_ROOT"
    rm -rf "$TMP_FAT"
}

trap cleanup EXIT

dd if=/dev/zero of="$DISK_IMG" bs=512M count=6 status=none
cat fdisk.args | fdisk "$DISK_IMG" >/dev/null

mkdir -p "$TMP_ROOT/etc" "$TMP_ROOT/boot" "$TMP_ROOT/bin"
printf "hello from ext4 root\n" > "$TMP_ROOT/hello.txt"
printf "rewrite me via kernel vfs\n" > "$TMP_ROOT/rw-demo.txt"
printf "nested file\n" > "$TMP_ROOT/etc/info.txt"
printf "mount point for fat32\n" > "$TMP_ROOT/boot/README"

if [ -n "${USER_BINS_DIR:-}" ] && [ -d "$USER_BINS_DIR" ]; then
    find "$USER_BINS_DIR" -maxdepth 1 -type f -exec cp {} "$TMP_ROOT/bin/" \;
    find "$TMP_ROOT/bin" -maxdepth 1 -type f -exec chmod 755 {} \;
    if [ -f "$TMP_ROOT/bin/python3" ] && [ ! -f "$TMP_ROOT/bin/python" ]; then
        cp "$TMP_ROOT/bin/python3" "$TMP_ROOT/bin/python"
        chmod 755 "$TMP_ROOT/bin/python"
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
    mkdir -p "$TMP_ROOT/usr/local/lib/python3.10"
    cp -a "$PYTHON_LIB_DIR"/. "$TMP_ROOT/usr/local/lib/python3.10/"
    find "$TMP_ROOT/usr/local/lib/python3.10" -type d -name "__pycache__" -prune -exec rm -rf {} +
    rm -rf "$TMP_ROOT/usr/local/lib/python3.10/test" \
        "$TMP_ROOT/usr/local/lib/python3.10/idlelib" \
        "$TMP_ROOT/usr/local/lib/python3.10/tkinter" \
        "$TMP_ROOT/usr/local/lib/python3.10/lib2to3" 2>/dev/null || true
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
