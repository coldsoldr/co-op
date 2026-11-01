# Phase 2 milestone 1: the coop audio-callback wrapper (identity wrap + measure).
# Build (WSL): make -C core/sdram ; output build/coop_cb.elf, injected into region A.
BFIN_BIN ?= $(HOME)/op1-toolchain/bfin/bin
BFIN_CXX := $(BFIN_BIN)/bfin-elf-g++
BUILD    ?= ../../build/core
CXXFLAGS := -O2 -std=c++17 -mcpu=bf524 -ffreestanding -fno-exceptions -fno-rtti \
            -fno-threadsafe-statics -fno-use-cxa-atexit -ffunction-sections -fdata-sections -Wall -Wextra
LDFLAGS  := -nostdlib -nostartfiles -T coop_cb.ld -Wl,--gc-sections -Wl,--no-undefined

.PHONY: all clean combined
all: $(BUILD)/coop_cb.elf
$(BUILD)/coop_cb.elf: coop_cb.cc coop_cb.ld | $(BUILD)
	$(BFIN_CXX) $(CXXFLAGS) coop_cb.cc $(LDFLAGS) -o $@ -lgcc
	$(BFIN_BIN)/bfin-elf-size $@

# Combined baseline+suite image: m1's whole-callback timer (CoopAudioCb, imm-seam patched)
# plus the master suite (coop_master_apply, wired by tools/l1_insert.py), one header, region A.
combined: $(BUILD)/coop_combined.elf
$(BUILD)/coop_combined.elf: coop_cb.cc master_suite.cc coop_cb.ld | $(BUILD)
	$(BFIN_CXX) $(CXXFLAGS) -DCOOP_COMBINED -I../include coop_cb.cc master_suite.cc $(LDFLAGS) -Wl,--undefined=_coop_master_apply -o $@ -lgcc
	$(BFIN_BIN)/bfin-elf-size $@
	$(BFIN_BIN)/bfin-elf-nm $@ | grep -iE "CoopAudioCb|coop_master_apply|g_max|g_blocks|g_master" | sort
$(BUILD):
	mkdir -p $(BUILD)
clean:
	rm -f $(BUILD)/coop_cb.elf
