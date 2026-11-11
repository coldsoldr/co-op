# The tape echo port's host test (tape_echo_compare.cc): plugins/tape_echo against te-op1-cfw's ports/fx tape echo,
# bit for bit, on the host (both in C) and in the Blackfin simulator (coop's assembly against the original's C).
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/tape_echo'          host
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/tape_echo sim'      simulator
#   ... make -C tests/host/tape_echo tables      regenerate plugins/tape_echo/src/tape_echo_tables.h
HOST_CXX ?= g++
TE ?= ../../../../../te-op1-cfw/ports
COOP := ../../../plugins/tape_echo
BUILD ?= ../../../build/host/tape_echo
BLOCKS ?= 20000
SIM_BLOCKS ?= 3000
CXXFLAGS := -O2 -std=c++17 -Wall -Wno-unused-function
REF_INC := -I$(TE)/fx -I$(TE) -I$(TE)/rings -I$(TE)/clouds
REF_SRC := $(TE)/fx/src/tape_echo/tape_echo.cc $(TE)/clouds/src/core/halfband.cc $(TE)/clouds/src/core/clouds_tables.cc \
           $(TE)/rings/src/fixed_tables.cc
COOP_SRC := $(COOP)/tape_echo.cc $(COOP)/src/tape_echo_core.cc $(COOP)/src/halfband.cc
COOP_DEPS := $(wildcard $(COOP)/*.cc $(COOP)/src/*)

BFIN ?= $(HOME)/op1-toolchain/bfin/bin
SIMFLAGS := -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -fno-tree-loop-distribute-patterns

.PHONY: all sim tables clean
all: $(BUILD)/tape_echo_ref $(BUILD)/tape_echo_coop
	$(BUILD)/tape_echo_ref $(BUILD)/ref.raw $(BLOCKS) | tee $(BUILD)/ref.txt
	$(BUILD)/tape_echo_coop $(BUILD)/coop.raw $(BLOCKS) | tee $(BUILD)/coop.txt
	cmp $(BUILD)/ref.txt $(BUILD)/coop.txt && echo "tape_echo: the sync divisions' time words agree"
	cmp $(BUILD)/ref.raw $(BUILD)/coop.raw && echo "tape_echo: coop == te-op1-cfw, bit-identical ($(BLOCKS) blocks, host)"

$(BUILD)/tape_echo_ref: tape_echo_compare.cc
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -Wno-class-memaccess -DTE_REF -DOP1_HOST_SIM $(REF_INC) tape_echo_compare.cc $(REF_SRC) -o $@

$(BUILD)/tape_echo_coop: tape_echo_compare.cc $(COOP_DEPS)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -I$(COOP) -I../../../sdk tape_echo_compare.cc $(COOP_SRC) -o $@

# The simulator: coop as it ships (its .S kernels; the clock stepped by the test), the original in C.
$(BUILD)/tape_echo_ref.elf: tape_echo_compare.cc
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -Wno-class-memaccess -DTE_REF -DOP1_HOST_SIM -DOP1FX_NO_ASM -DOP1CLOUDS_NO_ASM \
	  $(REF_INC) tape_echo_compare.cc $(REF_SRC) $(TE)/fx/src/tape_echo/tape_echo_bfin.S $(TE)/clouds/src/core/halfband_bfin.S -o $@

$(BUILD)/tape_echo_coop.elf: tape_echo_compare.cc $(COOP_DEPS)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -DCOOP_TAPE_ECHO_HOST_CLOCK -I$(COOP) -I../../../sdk tape_echo_compare.cc $(COOP_SRC) \
	  $(COOP)/src/tape_echo_bfin.S $(COOP)/src/halfband_bfin.S -o $@

sim: $(BUILD)/tape_echo_ref.elf $(BUILD)/tape_echo_coop.elf
	$(BFIN)/bfin-elf-run $(BUILD)/tape_echo_ref.elf $(BUILD)/ref_sim.raw $(SIM_BLOCKS)
	$(BFIN)/bfin-elf-run $(BUILD)/tape_echo_coop.elf $(BUILD)/coop_sim.raw $(SIM_BLOCKS)
	cmp $(BUILD)/ref_sim.raw $(BUILD)/coop_sim.raw && echo "tape_echo: coop (asm) == te-op1-cfw (C), bit-identical ($(SIM_BLOCKS) blocks, simulator)"

tables: gen_tables.cc
	@mkdir -p $(BUILD)
	$(HOST_CXX) -O2 -std=c++17 -Wno-unused-function -I$(TE)/fx -I$(TE) -I$(TE)/clouds -I$(TE)/rings gen_tables.cc \
	  $(TE)/clouds/src/core/halfband.cc $(TE)/rings/src/fixed_tables.cc -o $(BUILD)/gen_tables
	$(BUILD)/gen_tables > $(COOP)/src/tape_echo_tables.h

clean:
	rm -rf $(BUILD)
