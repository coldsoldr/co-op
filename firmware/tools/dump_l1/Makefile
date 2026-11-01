# Build the L1 dumper ELF (Phase 1). Run under WSL:
#   wsl -e bash -lc 'cd /i/GitHub/coldsoldr/coop-fw/tools/dump_l1 && make'
BFIN_BIN ?= $(HOME)/op1-toolchain/bfin/bin
BFIN_CXX := $(BFIN_BIN)/bfin-elf-g++
BUILD    ?= build
INCLUDE  := -I../../core/include

CXXFLAGS := -O2 -std=c++17 -mcpu=bf524 -ffreestanding -fno-exceptions -fno-rtti \
            -fno-threadsafe-statics -fno-use-cxa-atexit -ffunction-sections -fdata-sections \
            -fno-tree-loop-distribute-patterns -Wall -Wextra $(INCLUDE)
LDFLAGS  := -nostdlib -nostartfiles -T link.ld -Wl,--gc-sections -Wl,--no-undefined

.PHONY: all otp clean
all: $(BUILD)/dump_l1.elf

# Minimal dumper (default): context-block key + L1. Flash this first.
$(BUILD)/dump_l1.elf: dump_l1.cc link.ld ../../core/include/fw_addrs.h | $(BUILD)
	$(BFIN_CXX) $(CXXFLAGS) dump_l1.cc $(LDFLAGS) -o $@ -lgcc
	$(BFIN_BIN)/bfin-elf-size $@

# OTP dumper: also reads the whole OTP through the boot ROM, for op1emu's otp.bin.
otp: $(BUILD)/dump_l1_otp.elf
$(BUILD)/dump_l1_otp.elf: dump_l1.cc link.ld ../../core/include/fw_addrs.h | $(BUILD)
	$(BFIN_CXX) $(CXXFLAGS) -DDUMP_OTP dump_l1.cc $(LDFLAGS) -o $@ -lgcc
	$(BFIN_BIN)/bfin-elf-size $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
