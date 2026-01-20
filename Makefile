ARCH            :=  arm64
CROSS_COMPILE   :=  aarch64-none-linux-gnu-
BUILD_DIR       :=  build
BOOT_DIR        :=  boot
BIOS            :=  $(BOOT_DIR)/u-boot.bin
DISK_IMG        :=  $(BOOT_DIR)/disk.img
MOUNT_DIR       :=  mnt

# Toolchain
CC              :=  $(CROSS_COMPILE)gcc
AS              :=  $(CROSS_COMPILE)as
LD              :=  $(CROSS_COMPILE)ld
OBJCOPY         :=  $(CROSS_COMPILE)objcopy
QEMU           :=  qemu-system-aarch64

# Compiler flags
CFLAGS          :=  -g -Wall -nostdlib -nostdinc -Iinclude
ASFLAGS         :=  -g -Iinclude
LDFLAGS         :=  -nostdlib

# Linker script
LD_SCRIPT      :=  kernel/stupidos-aarch64.ld

# Source files
SRC_DIRS       :=  kernel
C_SRCS         :=  $(shell find $(SRC_DIRS) -name "*.c")
S_SRCS         :=  $(shell find $(SRC_DIRS) -name "*.S")

# Object files
C_OBJS         :=  $(patsubst %.c, $(BUILD_DIR)/%.o, $(C_SRCS))
S_OBJS         :=  $(patsubst %.S, $(BUILD_DIR)/%.o, $(S_SRCS))
OBJS           :=  $(C_OBJS) $(S_OBJS)

# Targets
KERNEL_ELF     :=  $(BUILD_DIR)/stupidos.elf
KERNEL_BIN     :=  $(BOOT_DIR)/Image

# Default target
.PHONY: all
all: $(KERNEL_BIN)

# ============================================
# Build rules
# ============================================

# Create build directories
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -E -P $< -o $(@:.o=.i)
	$(AS) $(ASFLAGS) $(@:.o=.i) -o $@

# Link kernel
$(KERNEL_ELF): $(OBJS) $(LD_SCRIPT)
	$(LD) $(LDFLAGS) $(OBJS) -o $@ -T $(LD_SCRIPT)

# Create binary image
$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

# ============================================
# Disk image management
# ============================================

.PHONY: disk
disk:
	@echo "Creating disk image..."
	@make -C script
	@mv script/disk.img $(DISK_IMG)

.PHONY: mount-disk
mount-disk: $(DISK_IMG)
	@echo "Mounting disk image..."
	@mkdir -p $(MOUNT_DIR)
	@sudo losetup -fP --show $(DISK_IMG) | sudo tee /tmp/loopdev
	@sudo mount $$(cat /tmp/loopdev)p1 $(MOUNT_DIR)

.PHONY: umount-disk
umount-disk:
	@if mountpoint -q $(MOUNT_DIR); then \
		echo "Unmounting disk image..."; \
		sudo umount $(MOUNT_DIR); \
	fi
	@if [ -f /tmp/loopdev ]; then \
		sudo losetup -d $$(cat /tmp/loopdev); \
	fi
	@rm -rf $(MOUNT_DIR)

.PHONY: install
install: $(KERNEL_BIN) mount-disk
	@echo "Installing kernel..."
	@sudo cp $(KERNEL_BIN) $(KERNEL_ELF) $(MOUNT_DIR)
	@make umount-disk

# ============================================
# QEMU targets
# ============================================

.PHONY: run
run: $(KERNEL_BIN) $(DISK_IMG) install
	@echo "Starting QEMU..."
	$(QEMU) -machine virt \
		-cpu cortex-a72 \
		-smp 4 \
		-m 4G \
		-bios $(BIOS) \
		-drive file=$(DISK_IMG),if=virtio,format=raw \
		-netdev user,id=net0 \
		-device virtio-net-device,netdev=net0 \
		-nographic \
		-serial mon:stdio

.PHONY: debug
debug: $(KERNEL_BIN) $(DISK_IMG) install
	@echo "Starting QEMU for debugging..."
	@echo "Start GDB with: 'target remote localhost:1234' $(KERNEL_ELF)"
	$(QEMU) -machine virt \
		-cpu cortex-a72 \
		-smp 4 \
		-m 4G \
		-bios $(BIOS) \
		-drive file=$(DISK_IMG),if=virtio,format=raw \
		-nographic \
		-serial mon:stdio \
		-S -s

# ============================================
# Cleanup
# ============================================

.PHONY: clean
clean:
	@echo "Cleaning up..."
	@rm -rf $(BUILD_DIR) $(MOUNT_DIR)
	@sudo rm -rf /tmp/loopdev


# ============================================
# Help
# ============================================

.PHONY: help
help:
	@echo "AArch64 Stupidos Kernel Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all         - Build kernel binary (default)"
	@echo "  disk        - Create disk image"
	@echo "  install     - Install kernel to disk image"
	@echo "  run         - Run kernel in QEMU"
	@echo "  debug       - Run kernel in debug mode"
	@echo "  clean       - Clean files"
	@echo "  help        - Show this help"
