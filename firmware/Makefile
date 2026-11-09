# coop-fw top-level Makefile.
# Firmware and plugins build with the bfin-elf toolchain (WSL); host tests build natively.
# From Windows:  wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make core'
# Stock firmware is never in the repo: point STOCK at your own unpacked op1_246 directory.

BFIN_BIN  ?= $(HOME)/op1-toolchain/bfin/bin
BFIN_CC   := $(BFIN_BIN)/bfin-elf-gcc
BFIN_CXX  := $(BFIN_BIN)/bfin-elf-g++
BFIN_LD   := $(BFIN_BIN)/bfin-elf-ld
BFIN_OBJCOPY := $(BFIN_BIN)/bfin-elf-objcopy
BFIN_NM   := $(BFIN_BIN)/bfin-elf-nm
BFIN_RUN  := $(BFIN_BIN)/bfin-elf-run
PYTHON    ?= python3
HOST_CXX  ?= g++
HOST_CC   ?= gcc

STOCK     ?= ../../te-op1-cfw/op1\ firmware/op1_246
BUILD     := build

# Freestanding Blackfin flags (same as te-op1-cfw/tools/knl_pack.py and kennel's pack.py).
BFIN_CFLAGS   := -O2 -mcpu=bf524 -ffreestanding -fno-builtin -nostdlib -fno-common \
                 -ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns -Wall -Wextra
BFIN_CXXFLAGS := $(BFIN_CFLAGS) -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
HOST_CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -DOP1_HOST
HOST_CFLAGS   := -O2 -std=c11 -Wall -Wextra -DOP1_HOST

.PHONY: all host-test sim-test core plugins package clean tools-test dumper

all: host-test

# ---- host tests: Python tool tests + C/C++ unit tests under tests/host ----
host-test: tools-test
	@$(MAKE) -C tests/host HOST_CXX="$(HOST_CXX)" HOST_CC="$(HOST_CC)" HOST_CXXFLAGS="$(HOST_CXXFLAGS)" HOST_CFLAGS="$(HOST_CFLAGS)"

# ---- sim test: verify the device (Blackfin) DSP codegen against the int64 reference ----
# Needs the bfin-elf toolchain + simulator (bfin-elf-run); confirms coop_mul_fr / the biquad's
# hardware-MAC path is bit-identical to the portable reference the host tests validate.
sim-test:
	BFIN_BIN="$(BFIN_BIN)" bash tests/sim/run_fr_check.sh

tools-test:
	$(PYTHON) -m unittest discover -s tests/tools -p 'test_*.py' -v

# ---- Phase 1: the L1 dumper payload ----
dumper:
	@$(MAKE) -C tools/dump_l1 BFIN_BIN="$(BFIN_BIN)" BUILD="$(abspath $(BUILD))/dump_l1"

# ---- core and plugins (Phase 2 onward) ----
core:
	@echo "core: nothing to build yet (Phase 2)"; exit 0

plugins:
	@echo "plugins: nothing to build yet (Phase 4)"; exit 0

# ---- package: patch the user's own stock image, pack and verify ----
# Usage: make package STOCK="/path/to/op1_246" IMAGE=tools/dump_l1/build/dump_l1.elf OUT=build/coop.op1 \n#            REGION_FLAG="--region 0x01e00000:0x02000000"
# REGION_FLAG defaults to region C (where the dumper and the coop core link).
REGION_FLAG ?= --region 0x01e00000:0x02000000
package:
	@test -n "$(IMAGE)" || { echo "IMAGE=... (an ELF linked for the injection region) is required"; exit 2; }
	@test -n "$(OUT)" || { echo "OUT=... (the .op1 to write) is required"; exit 2; }
	mkdir -p $(BUILD)/pkg
	rm -rf $(BUILD)/pkg/work && cp -r "$(STOCK)" $(BUILD)/pkg/work
	$(PYTHON) tools/inject_app.py "$(STOCK)/OP1_vdk.ldr" $(BUILD)/pkg/work/OP1_vdk.ldr "$(IMAGE)" $(REGION_FLAG) $(INJECT_FLAGS)
	$(PYTHON) tools/build_package.py $(BUILD)/pkg/work "$(OUT)"
	$(PYTHON) tools/verify_bootloader.py "$(OUT)"
	$(PYTHON) tools/pkg_check.py "$(OUT)" "$(STOCK)"

clean:
	rm -rf $(BUILD)
	@$(MAKE) -C tests/host clean 2>/dev/null || true
