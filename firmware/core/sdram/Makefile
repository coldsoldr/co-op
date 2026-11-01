# Phase 2 milestone 1: the coop audio-callback wrapper (identity wrap + measure).
# Build (WSL): make -C core/sdram ; output build/coop_cb.elf, injected into region A.
BFIN_BIN ?= $(HOME)/op1-toolchain/bfin/bin
BFIN_CXX := $(BFIN_BIN)/bfin-elf-g++
BUILD    ?= ../../build/core
CXXFLAGS := -O2 -std=c++17 -mcpu=bf524 -ffreestanding -fno-exceptions -fno-rtti \
            -fno-threadsafe-statics -fno-use-cxa-atexit -ffunction-sections -fdata-sections -Wall -Wextra
LDFLAGS  := -nostdlib -nostartfiles -T coop_cb.ld -Wl,--gc-sections -Wl,--no-undefined

.PHONY: all clean
all: $(BUILD)/coop_cb.elf
$(BUILD)/coop_cb.elf: coop_cb.cc coop_cb.ld | $(BUILD)
	$(BFIN_CXX) $(CXXFLAGS) coop_cb.cc $(LDFLAGS) -o $@ -lgcc
	$(BFIN_BIN)/bfin-elf-size $@
$(BUILD):
	mkdir -p $(BUILD)
clean:
	rm -f $(BUILD)/coop_cb.elf
