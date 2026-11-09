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

.PHONY: all otp full canary clean
all: $(BUILD)/dump_l1.elf

# Canary: only forwards to the stock service. Flash this first after a hang to prove the hook boots.
canary: $(BUILD)/dump_l1_canary.elf
$(BUILD)/dump_l1_canary.elf: dump_l1.cc link.ld ../../core/include/fw_addrs.h | $(BUILD)
	$(BFIN_CXX) $(CXXFLAGS) -DCANARY -Wno-unused-function dump_l1.cc $(LDFLAGS) -o $@ -lgcc
	$(BFIN_BIN)/bfin-elf-size $@

# Minimal dumper (default): context-block key + L1 data (DMA-free). Flash after the canary.
$(BUILD)/dump_l1.elf: dump_l1.cc link.ld ../../core/include/fw_addrs.h | $(BUILD)
	$(BFIN_CXX) $(CXXFLAGS) dump_l1.cc $(LDFLAGS) -o $@ -lgcc
	$(BFIN_BIN)/bfin-elf-size $@

# OTP dumper: also reads the whole OTP through the boot ROM, for op1emu's otp.bin.
otp: $(BUILD)/dump_l1_otp.elf
$(BUILD)/dump_l1_otp.elf: dump_l1.cc link.ld ../../core/include/fw_addrs.h | $(BUILD)
	$(BFIN_CXX) $(CXXFLAGS) -DDUMP_OTP dump_l1.cc $(LDFLAGS) -o $@ -lgcc
	$(BFIN_BIN)/bfin-elf-size $@

# Full dumper: OTP + L1 instruction SRAM via MDMA1 (device-vs-emulator cross-check). MDMA1 only;
# never MDMA0 (the display engine).
full: $(BUILD)/dump_l1_full.elf
$(BUILD)/dump_l1_full.elf: dump_l1.cc link.ld ../../core/include/fw_addrs.h | $(BUILD)
	$(BFIN_CXX) $(CXXFLAGS) -DDUMP_OTP -DDUMP_L1CODE dump_l1.cc $(LDFLAGS) -o $@ -lgcc
	$(BFIN_BIN)/bfin-elf-size $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
