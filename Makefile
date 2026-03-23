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
USER_DIR        :=  userspace
USER_INC_DIR    :=  $(USER_DIR)/include
USER_LD_SCRIPT  :=  $(USER_DIR)/user.ld
USER_BIN_DIR    :=  $(BUILD_DIR)/$(USER_DIR)/bin
PYTHON_BUILD_DIR := third_party/cpython310-build
PYTHON_LIBPY    :=  $(PYTHON_BUILD_DIR)/libpython3.10.a
PYTHON_LIB_DIR  :=  third_party/cpython310/Lib
PYTHON_INC_DIR  :=  third_party/cpython310/Include
USER_PROGRAMS   :=  hello ls cat ping sleep netcfg sh python3

# Toolchain
CC              :=  $(CROSS_COMPILE)gcc
AS              :=  $(CROSS_COMPILE)as
LD              :=  $(CROSS_COMPILE)ld
OBJCOPY         :=  $(CROSS_COMPILE)objcopy
OBJDUMP         :=  $(CROSS_COMPILE)objdump
DEBUGFILE		:= debug.s
GDB_SCRIPT      := $(BUILD_DIR)/stupidos-debug.gdb

QEMU           :=  qemu-system-aarch64
NET_MODE       ?=  user
TAP_IF         ?=  stupidos-tap
NET_DUMP       ?=
HOSTFWD_SSH    ?=
comma          :=  ,
ifeq ($(strip $(HOSTFWD_SSH)),)
QEMU_NET_USER  :=  -device virtio-net-device,netdev=net0 -netdev user,id=net0
else
QEMU_NET_USER  :=  -device virtio-net-device,netdev=net0 -netdev user,id=net0,hostfwd=$(HOSTFWD_SSH)
endif
QEMU_NET_TAP   :=  -netdev tap,id=net0,ifname=$(TAP_IF),script=no,downscript=no -device virtio-net-device,netdev=net0
QEMU_NET_ARGS  =  $(if $(filter tap,$(NET_MODE)),$(QEMU_NET_TAP),$(QEMU_NET_USER))
QEMU_NET_DUMP  =  $(if $(strip $(NET_DUMP)),-object filter-dump$(comma)id=netdump$(comma)netdev=net0$(comma)file=$(NET_DUMP))

# Compiler flags
CFLAGS          :=  -g -Wall -fno-builtin -Iinclude -O0 -march=armv8-a+nofp
ASFLAGS         :=  -g -Iinclude
LDFLAGS         :=  -nostdlib
DEPFLAGS        :=  -MMD -MP
USER_CFLAGS     :=  -g -Wall -fno-builtin -ffreestanding -fno-stack-protector -I$(USER_INC_DIR) -Iinclude -I$(PYTHON_INC_DIR) -I$(PYTHON_BUILD_DIR) -O0 -march=armv8-a+fp+simd
USER_ASFLAGS    :=  -g -Iinclude -I$(USER_INC_DIR)

# Linker script
LD_SCRIPT      :=  kernel/stupidos-aarch64.ld

# Source files
SRC_DIRS       :=  kernel
C_SRCS         :=  $(filter-out kernel/font.c,$(shell find $(SRC_DIRS) -name "*.c"))
S_SRCS         :=  $(shell find $(SRC_DIRS) -name "*.S")
USER_LIB_C_SRCS := $(shell find $(USER_DIR)/lib -name "*.c")
USER_BIN_C_SRCS := $(addprefix $(USER_DIR)/bin/,$(addsuffix .c,$(USER_PROGRAMS)))
USER_S_SRCS     := $(USER_DIR)/crt0.S $(USER_DIR)/lib/syscall_abi.S

# Object files
C_OBJS         :=  $(patsubst %.c, $(BUILD_DIR)/%.o, $(C_SRCS))
S_OBJS         :=  $(patsubst %.S, $(BUILD_DIR)/%.o, $(S_SRCS))
FONT_OBJS      :=  $(FONT_GEN_SRC:.c=.o)
OBJS           :=  $(C_OBJS) $(FONT_OBJS) $(S_OBJS)
DEPS           :=  $(C_OBJS:.o=.d) $(FONT_OBJS:.o=.d)
USER_LIB_OBJS  :=  $(patsubst $(USER_DIR)/%.c,$(BUILD_DIR)/$(USER_DIR)/%.o,$(USER_LIB_C_SRCS))
USER_BIN_OBJS  :=  $(patsubst $(USER_DIR)/%.c,$(BUILD_DIR)/$(USER_DIR)/%.o,$(USER_BIN_C_SRCS))
USER_CRT_OBJ   :=  $(patsubst $(USER_DIR)/%.S,$(BUILD_DIR)/$(USER_DIR)/%.o,$(USER_S_SRCS))
USER_BINS      :=  $(addprefix $(USER_BIN_DIR)/,$(USER_PROGRAMS))
DEPS           +=  $(USER_LIB_OBJS:.o=.d) $(USER_BIN_OBJS:.o=.d) $(USER_CRT_OBJ:.o=.d)

-include $(DEPS)

# Targets
KERNEL_ELF     :=  $(BUILD_DIR)/stupidos.elf
KERNEL_KIMAGE_ELF := $(BUILD_DIR)/stupidos-kimage.elf
KERNEL_BIN     :=  $(BOOT_DIR)/Image
KIMAGE_OFFSET  :=  0xffff7fffd0000000

# Default target
.PHONY: all
.SECONDARY: $(KERNEL_KIMAGE_ELF) $(GDB_SCRIPT) $(USER_CRT_OBJ)
all: $(KERNEL_BIN)
all: $(USER_BINS)

# ============================================
# Build rules
# ============================================

# Create build directories
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -E -P $< -o $(@:.o=.i)
	$(AS) $(ASFLAGS) $(@:.o=.i) -o $@

$(BUILD_DIR)/$(USER_DIR)/%.o: $(USER_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/$(USER_DIR)/%.o: $(USER_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -E -P $< -o $(@:.o=.i)
	$(AS) $(USER_ASFLAGS) $(@:.o=.i) -o $@

$(USER_BIN_DIR)/%: $(BUILD_DIR)/$(USER_DIR)/bin/%.o $(USER_LIB_OBJS) $(USER_CRT_OBJ) $(USER_LD_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD) -nostdlib -z max-page-size=0x1000 $(USER_CRT_OBJ) $(USER_LIB_OBJS) $< -o $@ -T $(USER_LD_SCRIPT)

$(USER_BIN_DIR)/python3: $(BUILD_DIR)/$(USER_DIR)/bin/python3.o $(USER_LIB_OBJS) $(USER_CRT_OBJ) $(USER_LD_SCRIPT) $(PYTHON_LIBPY)
	@mkdir -p $(dir $@)
	$(LD) -nostdlib -z max-page-size=0x1000 $(USER_CRT_OBJ) $(USER_LIB_OBJS) $< $(PYTHON_LIBPY) -o $@ -T $(USER_LD_SCRIPT)

$(PYTHON_LIBPY):
	@$(MAKE) -C $(PYTHON_BUILD_DIR) -j2 V=1 libpython3.10.a

$(FONT_GEN_SRC): $(FONT_TTF) tools/gen_font.py
	@mkdir -p $(dir $@)
	python3 tools/gen_font.py --ttf $(FONT_TTF) --out $@

$(FONT_GEN_SRC:.c=.o): $(FONT_GEN_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

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

$(DISK_IMG): $(USER_BINS) script/disk.sh script/Makefile script/fdisk.args
	@echo "Creating disk image..."
	@rm -f $(DISK_IMG)
	@$(MAKE) -C script \
		USER_BINS_DIR="$(abspath $(USER_BIN_DIR))" \
		PYTHON_BIN="$(abspath $(USER_BIN_DIR)/python3)" \
		PYTHON_LIB_DIR="$(abspath $(PYTHON_LIB_DIR))"
	@mv script/disk.img $(DISK_IMG)

.PHONY: disk
disk: $(DISK_IMG)

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
run: $(KERNEL_BIN) $(QEMU_DTB) $(DISK_IMG)
	@echo "Starting QEMU..."
	@echo "Serial shell is on this terminal; RAMFB/UI is shown in the GTK window."
	$(QEMU) -M virt \
		-cpu cortex-a72 \
		-smp 1 \
		-m 1G \
		-rtc base=utc,clock=host \
		-global virtio-mmio.force-legacy=false \
	-drive file=$(DISK_IMG),if=none,format=raw,id=hd0 \
	-device virtio-blk-device,drive=hd0 \
	-device virtio-keyboard-device -device virtio-tablet-device \
	-device ramfb \
	-display gtk \
	$(QEMU_NET_DUMP) \
	$(QEMU_NET_ARGS) \
	-kernel $(KERNEL_BIN) \
	-dtb $(QEMU_DTB) \
	-serial stdio \
	-monitor none

.PHONY: run-user
run-user: NET_MODE=user
run-user: run

.PHONY: run-headless
run-headless: $(KERNEL_BIN) $(QEMU_DTB) $(DISK_IMG)
	@echo "Starting QEMU (headless)..."
	@echo "Headless mode uses this terminal for the serial shell."
	$(QEMU) -M virt \
		-cpu cortex-a72 \
		-smp 1 \
		-m 1G \
		-rtc base=utc,clock=host \
		-global virtio-mmio.force-legacy=false \
	-drive file=$(DISK_IMG),if=none,format=raw,id=hd0 \
	-device virtio-blk-device,drive=hd0 \
	-device virtio-keyboard-device -device virtio-tablet-device \
	$(QEMU_NET_DUMP) \
	$(QEMU_NET_ARGS) \
	-kernel $(KERNEL_BIN) \
	-dtb $(QEMU_DTB) \
	-nographic \
	-monitor none

.PHONY: run-tap
run-tap: NET_MODE=tap
run-tap: run

.PHONY: tap-setup
tap-setup:
	@echo "Setting up TAP device $(TAP_IF)..."
	@sudo ip tuntap add dev $(TAP_IF) mode tap || true
	@sudo ip link set dev $(TAP_IF) address 12:34:56:65:43:21 || true
	@sudo ip link set $(TAP_IF) up

.PHONY: tap-clean
tap-clean:
	@echo "Removing TAP device $(TAP_IF)..."
	@sudo ip link set $(TAP_IF) down || true
	@sudo ip tuntap del dev $(TAP_IF) mode tap || true

.PHONY: debug

debug: $(KERNEL_BIN) $(KERNEL_KIMAGE_ELF) $(GDB_SCRIPT) $(DISK_IMG)
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
	$(QEMU_NET_DUMP) \
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
	@echo "  run-user    - Run kernel with usernet + hostfwd"
	@echo "  run-tap     - Run kernel in QEMU with TAP networking (NET_MODE=tap)"
	@echo "  tap-setup   - Create and bring up TAP device $(TAP_IF)"
	@echo "  tap-clean   - Remove TAP device $(TAP_IF)"
	@echo "  debug       - Run kernel in debug mode"
	@echo "  clean       - Clean files"
	@echo "  help        - Show this help"
