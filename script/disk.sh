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

cleanup() {
    rm -rf "$TMP_ROOT"
    rm -rf "$TMP_FAT"
}

trap cleanup EXIT

dd if=/dev/zero of="$DISK_IMG" bs=512M count=6 status=none
cat fdisk.args | fdisk "$DISK_IMG" >/dev/null

mkdir -p "$TMP_ROOT/etc" "$TMP_ROOT/boot"
printf "hello from ext4 root\n" > "$TMP_ROOT/hello.txt"
printf "rewrite me via kernel vfs\n" > "$TMP_ROOT/rw-demo.txt"
printf "nested file\n" > "$TMP_ROOT/etc/info.txt"
printf "mount point for fat32\n" > "$TMP_ROOT/boot/README"

printf "hello from fat32 boot volume\n" > "$TMP_FAT/README.TXT"
printf "kernel overwrites me via fat32\n" > "$TMP_FAT/RWDEMO.TXT"
printf "fat32 nested file\n" > "$TMP_FAT/INFO.TXT"

mkfs.fat -F 32 -s 8 --offset="$P1_START" "$DISK_IMG" $((P1_SECTORS / 2)) >/dev/null 2>&1
mmd -i "$DISK_IMG@@$P1_OFFSET" ::/EFI >/dev/null
mmd -i "$DISK_IMG@@$P1_OFFSET" ::/EFI/BOOT >/dev/null
mcopy -i "$DISK_IMG@@$P1_OFFSET" "$TMP_FAT/README.TXT" ::/README.TXT >/dev/null
mcopy -i "$DISK_IMG@@$P1_OFFSET" "$TMP_FAT/RWDEMO.TXT" ::/RWDEMO.TXT >/dev/null
mcopy -i "$DISK_IMG@@$P1_OFFSET" "$TMP_FAT/INFO.TXT" ::/EFI/BOOT/INFO.TXT >/dev/null
mkfs.ext4 -F -q -b "$EXT4_BLOCK_SIZE" -d "$TMP_ROOT" \
    -E "offset=$P2_OFFSET" "$DISK_IMG" "$EXT4_BLOCKS"
