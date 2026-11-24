# Fresh OS build
#   make            build build/fresh-os.img (BIOS + UEFI bootable disk image)
#   make run        boot it in QEMU (BIOS);  make run-uefi  boot with OVMF
#   make test       full test suite (host tests + QEMU end-to-end)
#   make CC=gcc     build kernel/userland with GCC instead of clang

BUILD   ?= build
CC      ?= clang
HOSTCC  ?= cc
NASM    ?= nasm
LD      := ld.lld
PYTHON  ?= python3
AR      := llvm-ar
CMDLINE ?=

ifeq ($(origin CC),default)
CC := clang
endif

IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -q clang && echo 1)
ifeq ($(IS_CLANG),1)
TARGET_FLAG := --target=x86_64-unknown-none-elf
else
TARGET_FLAG :=
endif
CC_INC := $(shell $(CC) -print-file-name=include)

WARN   := -Wall -Wextra -Werror -Wno-unused-parameter
COMMON := $(TARGET_FLAG) -std=gnu11 -O2 -g -ffreestanding -nostdinc -isystem $(CC_INC) \
          -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables \
          -fno-omit-frame-pointer -fno-strict-aliasing $(WARN) -Iinclude

KCFLAGS := $(COMMON) -DFRESH_KERNEL -mcmodel=kernel -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387 -Ikernel -Ilib
UCFLAGS := $(COMMON) -Iuser/libc/include -Ilib
LDFLAGS := -nostdlib -static -z max-page-size=4096 --no-dynamic-linker

# ---------------------------------------------------------------- kernel
KERNEL_C   := $(shell find kernel -name '*.c') lib/common/string.c lib/common/fmt.c \
              $(wildcard lib/freshfs/*.c)
KERNEL_ASM := $(shell find kernel -name '*.asm')
KERNEL_OBJ := $(patsubst %.c,$(BUILD)/k/%.o,$(KERNEL_C)) \
              $(patsubst %.asm,$(BUILD)/k/%.o,$(KERNEL_ASM)) \
              $(BUILD)/k/gen/font_data.o
KERNEL_HDR := $(shell find kernel include lib -name '*.h')

$(BUILD)/k/%.o: %.c $(KERNEL_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/k/%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -g $< -o $@

$(BUILD)/gen/font_data.c: tools/font.txt tools/genfont.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/genfont.py tools/font.txt $@

$(BUILD)/k/gen/font_data.o: $(BUILD)/gen/font_data.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(KERNEL_OBJ) kernel/linker.ld
	$(LD) $(LDFLAGS) -T kernel/linker.ld -o $@ $(KERNEL_OBJ)

$(BUILD)/KERNEL.BIN: $(BUILD)/kernel.elf tools/mkkernel.py
	$(PYTHON) tools/mkkernel.py $< $@

# ---------------------------------------------------------------- loaders
$(BUILD)/stage1.bin: boot/bios/stage1.asm
	@mkdir -p $(BUILD)
	$(NASM) -f bin $< -o $@

$(BUILD)/stage2.bin: boot/bios/stage2.asm
	@mkdir -p $(BUILD)
	$(NASM) -f bin $< -o $@

EFI_CFLAGS := --target=x86_64-unknown-windows -std=gnu11 -O2 -ffreestanding -fshort-wchar \
              -mno-red-zone -mno-sse -fno-stack-protector -fno-builtin $(WARN) -Iinclude

$(BUILD)/uefi/main.o: boot/uefi/main.c boot/uefi/efi.h include/fresh/bootinfo.h
	@mkdir -p $(dir $@)
	clang $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/BOOTX64.EFI: $(BUILD)/uefi/main.o
	lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /out:$@ $<

# ---------------------------------------------------------------- userland
LIBC_C   := $(wildcard user/libc/*.c) lib/common/string.c lib/common/fmt.c
LIBC_ASM := $(wildcard user/libc/*.asm)
LIBC_OBJ := $(patsubst %.c,$(BUILD)/u/%.o,$(LIBC_C)) $(patsubst %.asm,$(BUILD)/u/%.o,$(LIBC_ASM))
USER_HDR := $(shell find user include -name '*.h')
PROGS    := $(basename $(notdir $(wildcard user/bin/*.c)))
PROG_BIN := $(addprefix $(BUILD)/rootfs/bin/,$(PROGS))
TESTS    := $(basename $(notdir $(wildcard user/test/*.c)))
TEST_BIN := $(addprefix $(BUILD)/rootfs/test/,$(TESTS))

$(BUILD)/u/%.o: %.c $(USER_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/u/%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/libc.a: $(LIBC_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/rootfs/bin/%: $(BUILD)/u/user/bin/%.o $(BUILD)/libc.a user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -s -T user/user.ld -o $@ $(BUILD)/u/user/libc/crt0.o $< $(BUILD)/libc.a

$(BUILD)/rootfs/test/%: $(BUILD)/u/user/test/%.o $(BUILD)/libc.a user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -s -T user/user.ld -o $@ $(BUILD)/u/user/libc/crt0.o $< $(BUILD)/libc.a

# the desktop is built from several files plus the console font
DESKTOP_OBJ := $(patsubst %.c,$(BUILD)/u/%.o,$(wildcard user/desktop/*.c)) $(BUILD)/u/gen/font_data.o

$(BUILD)/u/gen/font_data.o: $(BUILD)/gen/font_data.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/rootfs/bin/desktop: $(DESKTOP_OBJ) $(BUILD)/libc.a user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -s -T user/user.ld -o $@ $(BUILD)/u/user/libc/crt0.o $(DESKTOP_OBJ) $(BUILD)/libc.a

ROOTFS_FILES := $(shell find rootfs -type f 2>/dev/null)

$(BUILD)/INITRD: $(PROG_BIN) $(TEST_BIN) $(BUILD)/rootfs/bin/desktop $(ROOTFS_FILES) tools/mkinitrd.py
	$(PYTHON) tools/mkinitrd.py $@ rootfs $(BUILD)/rootfs

# ---------------------------------------------------------------- host tools
FFS_SRC := $(wildcard lib/freshfs/*.c)
FFS_HDR := $(wildcard lib/freshfs/*.h)

$(BUILD)/tools/ffstool: tools/ffstool.c $(FFS_SRC) $(FFS_HDR)
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -g -Wall -Wextra -Werror -Ilib -Iinclude -o $@ tools/ffstool.c $(FFS_SRC)

# ---------------------------------------------------------------- image
IMAGE_DEPS := $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/KERNEL.BIN $(BUILD)/INITRD \
              $(BUILD)/BOOTX64.EFI $(BUILD)/tools/ffstool tools/mkimage.py $(shell find data -type f 2>/dev/null)

define mkimage
	$(PYTHON) tools/mkimage.py --stage1 $(BUILD)/stage1.bin --stage2 $(BUILD)/stage2.bin \
	  --kernel $(BUILD)/KERNEL.BIN --initrd $(BUILD)/INITRD --efi $(BUILD)/BOOTX64.EFI \
	  --ffstool $(BUILD)/tools/ffstool --data-dir data --cmdline "$(1)" -o $(2)
endef

$(BUILD)/fresh-os.img: $(IMAGE_DEPS)
	$(call mkimage,$(CMDLINE),$@)

$(BUILD)/fresh-os-test.img: $(IMAGE_DEPS)
	$(call mkimage,testmode,$@)

.PHONY: all image test-image gcc-image run run-uefi test host-test analyze clean
all: image
image: $(BUILD)/fresh-os.img
test-image: $(BUILD)/fresh-os-test.img

OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd

run: $(BUILD)/fresh-os.img
	qemu-system-x86_64 -machine q35 -m 512M -drive file=$<,format=raw -serial stdio

run-uefi: $(BUILD)/fresh-os.img
	cp $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	qemu-system-x86_64 -machine q35 -m 512M \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	  -drive file=$<,format=raw -serial stdio

gcc-image:
	$(MAKE) CC=gcc BUILD=build-gcc image

host-test: $(BUILD)/tools/ffstool $(BUILD)/INITRD
	$(MAKE) -C tests/host BUILD=$(abspath $(BUILD))/host

analyze:
	tests/analyze.sh

# full gate: host unit tests, static analysis, end-to-end tests in QEMU
SOAK ?= 600
test: image test-image gcc-image host-test analyze
	$(PYTHON) tests/e2e/run_all.py --build $(BUILD) --soak $(SOAK)

clean:
	rm -rf $(BUILD) build-gcc
