# Host-side unit tests (run with AddressSanitizer + UndefinedBehaviorSanitizer)
ROOT   := ../..
BUILD  ?= $(ROOT)/build/host
CC     ?= cc
SAN    := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS := -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter $(SAN) -I$(ROOT)/lib -I$(ROOT)/include
FFS    := $(ROOT)/lib/freshfs/freshfs.c $(ROOT)/lib/freshfs/fsck.c
RENAME := -Dmemcpy=f_memcpy -Dmemmove=f_memmove -Dmemset=f_memset -Dmemcmp=f_memcmp -Dstrlen=f_strlen \
          -Dstrnlen=f_strnlen -Dstrcmp=f_strcmp -Dstrncmp=f_strncmp -Dstrcpy=f_strcpy -Dstrncpy=f_strncpy \
          -Dstrcat=f_strcat -Dstrlcpy=f_strlcpy -Dstrlcat=f_strlcat -Dstrchr=f_strchr -Dstrrchr=f_strrchr \
          -Dstrstr=f_strstr -Dvsnprintf=fresh_vsnprintf -Dsnprintf=fresh_snprintf

.PHONY: all run
all: run

$(BUILD)/test_freshfs: test_freshfs.c $(FFS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/test_fsck_fuzz: test_fsck_fuzz.c $(FFS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/fresh_str.o: $(ROOT)/lib/common/string.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fno-builtin $(RENAME) -ffreestanding -c -o $@ $<

$(BUILD)/fresh_fmt.o: $(ROOT)/lib/common/fmt.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fno-builtin -Dvsnprintf=fresh_vsnprintf -Dsnprintf=fresh_snprintf -Dstrlen=f_strlen \
	  -Dstrnlen=f_strnlen -Wno-format-nonliteral -c -o $@ $<

$(BUILD)/test_fmt: test_fmt.c $(BUILD)/fresh_str.o $(BUILD)/fresh_fmt.o
	$(CC) $(CFLAGS) -Wno-format-truncation -o $@ $^

$(BUILD)/test_kernel_units: test_kernel_units.c $(ROOT)/kernel/mm/heap.c $(ROOT)/kernel/proc/elf.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DFRESH_HOST_TEST -I$(ROOT)/kernel -o $@ $^

run: $(BUILD)/test_freshfs $(BUILD)/test_fsck_fuzz $(BUILD)/test_fmt $(BUILD)/test_kernel_units
	$(BUILD)/test_fmt
	$(BUILD)/test_kernel_units $(ROOT)/build/rootfs/bin/echo
	$(BUILD)/test_freshfs
	$(BUILD)/test_fsck_fuzz 3000
