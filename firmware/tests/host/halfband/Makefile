# The shared half-band kernels' check (halfband_check.cc): plugins/shared/halfband_bfin.S against the C references
# (plugins/tape_echo/src/halfband.cc built for the Blackfin with COOP_TAPE_ECHO_C), in the GNU simulator.
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/halfband'
COOP := ../../../plugins
BUILD ?= ../../../build/host/halfband
BFIN ?= $(HOME)/op1-toolchain/bfin/bin
# te-op1-cfw's original kernels (what the plugins carried before), when the sibling checkout is there: renamed and
# linked for their cycles
TE ?= ../../../../../te-op1-cfw/ports
UP := $(wildcard $(TE)/clouds/src/core/halfband_bfin.S)
SIMFLAGS := -DCOOP_TAPE_ECHO_C -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -fno-tree-loop-distribute-patterns -Wall
SRC := halfband_check.cc $(COOP)/tape_echo/src/halfband.cc $(COOP)/shared/halfband_bfin.S

.PHONY: all clean
all: $(BUILD)/halfband_check.elf
	$(BFIN)/bfin-elf-run $<

$(BUILD)/halfband_check.elf: $(SRC) $(COOP)/tape_echo/src/half_rate.h
	@mkdir -p $(BUILD)
ifneq ($(UP),)
	sed -e 's/_Halfband/_UpHalfband/g' -e 's/_MixSoftLimitAsm/_UpMixSoftLimitAsm/g' $(UP) > $(BUILD)/upstream_bfin.S
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -DHB_UPSTREAM -I$(COOP)/tape_echo $(SRC) $(BUILD)/upstream_bfin.S -o $@
else
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -I$(COOP)/tape_echo $(SRC) -o $@
endif

clean:
	rm -rf $(BUILD)
