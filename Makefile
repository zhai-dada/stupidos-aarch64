ARCH            :=  arm64
CROSS_COMPILE   :=  aarch64-none-linux-gnu-
BUILD_DIR       :=  build
GEN_DIR         :=  $(BUILD_DIR)/generated
BOOT_DIR		:= 	boot
DISK_IMG        :=  $(BOOT_DIR)/disk.img
MOUNT_DIR       :=  mnt
FONT_TTF        :=  Monaco.ttf
FONT_GEN_SRC    :=  $(GEN_DIR)/font.c
QEMU_DTS        :=  doc/qemu-virt.dts
QEMU_DTB        :=  $(BUILD_DIR)/qemu-virt.dtb

# Toolchain
CC              :=  $(CROSS_COMPILE)gcc
AS              :=  $(CROSS_COMPILE)as
LD              :=  $(CROSS_COMPILE)ld
OBJCOPY         :=  $(CROSS_COMPILE)objcopy
OBJDUMP         :=  $(CROSS_COMPILE)objdump
DEBUGFILE		:= debug.s
GDB_SCRIPT      := $(BUILD_DIR)/stupidos-debug.gdb

QEMU           :=  qemu-system-aarch64

# Compiler flags
CFLAGS          :=  -g -Wall -fno-builtin -Iinclude -O0 -march=armv8-a+nofp
ASFLAGS         :=  -g -Iinclude
LDFLAGS         :=  -nostdlib

# Linker script
LD_SCRIPT      :=  kernel/stupidos-aarch64.ld

# Source files
SRC_DIRS       :=  kernel
C_SRCS         :=  $(filter-out kernel/font.c,$(shell find $(SRC_DIRS) -name "*.c"))
S_SRCS         :=  $(shell find $(SRC_DIRS) -name "*.S")

# Object files
C_OBJS         :=  $(patsubst %.c, $(BUILD_DIR)/%.o, $(C_SRCS))
S_OBJS         :=  $(patsubst %.S, $(BUILD_DIR)/%.o, $(S_SRCS))
FONT_OBJS      :=  $(FONT_GEN_SRC:.c=.o)
OBJS           :=  $(C_OBJS) $(FONT_OBJS) $(S_OBJS)

# Targets
KERNEL_ELF     :=  $(BUILD_DIR)/stupidos.elf
KERNEL_KIMAGE_ELF := $(BUILD_DIR)/stupidos-kimage.elf
KERNEL_BIN     :=  $(BOOT_DIR)/Image
KIMAGE_OFFSET  :=  0xffff7fffd0000000

# Default target
.PHONY: all
.SECONDARY: $(KERNEL_KIMAGE_ELF) $(GDB_SCRIPT)
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

$(FONT_GEN_SRC): $(FONT_TTF) tools/gen_font.py
	@mkdir -p $(dir $@)
	python3 tools/gen_font.py --ttf $(FONT_TTF) --out $@

$(FONT_GEN_SRC:.c=.o): $(FONT_GEN_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(QEMU_DTB): $(QEMU_DTS)
	@mkdir -p $(dir $@)
	dtc -I dts -O dtb $< -o $@

# Link kernel
$(KERNEL_ELF): $(OBJS) $(LD_SCRIPT)
	$(LD) $(LDFLAGS) $(OBJS) -o $@ -T $(LD_SCRIPT)

$(KERNEL_KIMAGE_ELF): $(KERNEL_ELF)
	$(OBJCOPY) --change-addresses=$(KIMAGE_OFFSET) $< $@

# Create binary image
$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

$(GDB_SCRIPT): $(KERNEL_ELF) $(KERNEL_KIMAGE_ELF)
	@printf "set pagination off\n" > $@
	@printf "set confirm off\n" >> $@
	@printf "set architecture aarch64\n" >> $@
	@printf 'set $$kimage_offset = %#x\n' $(KIMAGE_OFFSET) >> $@
	@printf "define low-symbols\n" >> $@
	@printf "  symbol-file %s\n" "$(abspath $(KERNEL_ELF))" >> $@
	@printf "end\n" >> $@
	@printf "define kimage-symbols\n" >> $@
	@printf "  symbol-file %s\n" "$(abspath $(KERNEL_KIMAGE_ELF))" >> $@
	@printf "end\n" >> $@
	@printf "define kbreak\n" >> $@
	@printf '  hbreak *($$arg0 + $$kimage_offset)\n' >> $@
	@printf "end\n" >> $@
	@printf "document low-symbols\n" >> $@
	@printf "Load the low-address ELF symbols used before the MMU jumps to KIMAGE_VADDR.\n" >> $@
	@printf "end\n" >> $@
	@printf "document kimage-symbols\n" >> $@
	@printf "Load the high-address ELF symbols used after the kernel starts running at KIMAGE_VADDR.\n" >> $@
	@printf "end\n" >> $@
	@printf "document kbreak\n" >> $@
	@printf "Set a breakpoint at the KIMAGE_VADDR alias of a low-linked symbol. Example: kbreak kernel_main_high\n" >> $@
	@printf "end\n" >> $@
	@printf "low-symbols\n" >> $@

# ============================================
# Disk image management
# ============================================

.PHONY: disk
disk:
	@echo "Creating disk image..."
	@make -C script
	@mv script/disk.img $(DISK_IMG)

.PHONY: mount-disk
mount-disk:
	@echo "Mounting disk image..."
	@mkdir -p $(MOUNT_DIR)
	@if [ -f $(DISK_IMG) ];then	\
		echo "no disk";			\
		else					\
		make disk;				\
	fi
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
run: $(KERNEL_BIN) $(QEMU_DTB)
	@if [ ! -f $(DISK_IMG) ]; then \
		$(MAKE) disk; \
	fi
	@echo "Starting QEMU..."
	$(QEMU) -M virt \
		-cpu cortex-a72 \
		-smp 4 \
		-m 1G \
		-rtc base=utc,clock=host \
		-global virtio-mmio.force-legacy=false \
		-drive file=$(DISK_IMG),if=none,format=raw,id=hd0 \
		-device virtio-blk-device,drive=hd0 \
		-device virtio-keyboard-device -device virtio-tablet-device \
		-device ramfb \
		-display gtk \
		-device virtio-net-device,netdev=net0 -netdev user,id=net0\
		-device virtio-net-pci,netdev=pcinet0,mac=52:54:00:12:34:56 -netdev user,id=pcinet0 \
		-device virtio-sound-device,audiodev=audio0 -audiodev sdl,id=audio0\
		-kernel $(KERNEL_BIN) \
		-dtb $(QEMU_DTB) \
		-serial mon:stdio

.PHONY: run-headless
run-headless: $(KERNEL_BIN) $(QEMU_DTB)
	@if [ ! -f $(DISK_IMG) ]; then \
		$(MAKE) disk; \
	fi
	@echo "Starting QEMU (headless)..."
	$(QEMU) -M virt \
		-cpu cortex-a72 \
		-smp 1 \
		-m 1G \
		-rtc base=utc,clock=host \
		-global virtio-mmio.force-legacy=false \
		-drive file=$(DISK_IMG),if=none,format=raw,id=hd0 \
		-device virtio-blk-device,drive=hd0 \
		-device virtio-keyboard-device -device virtio-tablet-device \
		-device virtio-net-device,netdev=net0 -netdev user,id=net0\
		-kernel $(KERNEL_BIN) \
		-dtb $(QEMU_DTB) \
		-nographic \
		-monitor none

.PHONY: debug

debug: $(KERNEL_BIN) $(KERNEL_KIMAGE_ELF) $(GDB_SCRIPT)
	@if [ ! -f $(DISK_IMG) ]; then \
		$(MAKE) disk; \
	fi
	@echo "Starting QEMU for debugging..."
	@echo "Start GDB and run:"
	@echo "  source $(GDB_SCRIPT)"
	@echo "  target remote localhost:1234"
	@echo "Before MMU switch, use low-symbols"
	@echo "After jumping to KIMAGE_VADDR, use kimage-symbols"
	@echo "To break on a high-address alias before switching symbols, use: kbreak kernel_main_high"
	$(OBJDUMP) -D ./build/stupidos.elf > $(DEBUGFILE)
	$(QEMU) -M virt \
		-cpu cortex-a72 \
		-smp 4 \
		-m 4G \
		-rtc base=utc,clock=host \
		-global virtio-mmio.force-legacy=false \
		-drive file=$(DISK_IMG),if=none,format=raw,id=hd0 \
		-device virtio-blk-device,drive=hd0 \
		-device virtio-keyboard-device -device virtio-tablet-device \
		-device ramfb \
		-display gtk \
		-device virtio-net-device,netdev=net0 -netdev user,id=net0\
		-device virtio-net-pci,netdev=pcinet0,mac=52:54:00:12:34:56 -netdev user,id=pcinet0 \
		-device virtio-sound-device,audiodev=audio0 -audiodev sdl,id=audio0\
		-kernel ./build/stupidos.elf \
		-serial mon:stdio \
		-S -s

# ============================================
# Cleanup
# ============================================

.PHONY: clean
clean:
	@echo "Cleaning up..."
	@rm -rf $(BUILD_DIR) $(MOUNT_DIR)
	@rm -rf $(DEBUGFILE)
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
