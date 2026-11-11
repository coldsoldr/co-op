# The cs_hall port's host test (cs_hall_compare.cc): plugins/cs_hall against te-op1-cfw's hall as its kennel build
# played it, bit for bit.
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/cs_hall'        host C, ~10 s
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/cs_hall sim'    + the Blackfin build
#                                                                     with its assembly in bfin-elf-run, and its cycles
HOST_CXX ?= g++
TE ?= ../../../../../te-op1-cfw/ports
COOP := ../../../plugins/cs_hall
BUILD ?= ../../../build/host/cs_hall
CXXFLAGS := -O2 -std=c++17 -Wall -Wno-unused-variable -Wno-unused-function
COOP_SRC := $(COOP)/cs_hall.cc $(COOP)/hall.cc $(COOP)/halfband.cc
REF_SRC := $(TE)/fx/src/hall/hall.cc $(TE)/clouds/src/core/halfband.cc $(TE)/clouds/src/core/clouds_tables.cc \
           $(TE)/rings/src/fixed_tables.cc
# hall_knl.cc's include order (ports/knl/cs_hall/plugin.json)
REF_INC := -I$(TE)/knl -I$(TE) -I$(TE)/fx -I$(TE)/rings -I$(TE)/clouds

BFIN ?= $(HOME)/op1-toolchain/bfin/bin
SIMFLAGS := -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -fno-tree-loop-distribute-patterns -Wall

.PHONY: all sim clean
all: $(BUILD)/hall_ref $(BUILD)/hall_coop
	$(BUILD)/hall_ref $(BUILD)/ref.raw
	$(BUILD)/hall_coop $(BUILD)/coop.raw > /dev/null
	cmp $(BUILD)/ref.raw $(BUILD)/coop.raw && echo "cs_hall: coop == te-op1-cfw (kennel glue), bit-identical"

sim: all $(BUILD)/hall_coop.elf
	$(BUILD)/hall_ref $(BUILD)/ref_nogap.raw --nogap > /dev/null
	$(BFIN)/bfin-elf-run $(BUILD)/hall_coop.elf $(BUILD)/sim.raw --nogap
	cmp $(BUILD)/ref_nogap.raw $(BUILD)/sim.raw && echo "cs_hall: coop on the Blackfin (assembly) == te-op1-cfw, bit-identical"

$(BUILD)/hall_ref: cs_hall_compare.cc $(REF_SRC)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -Wno-class-memaccess -DHALL_REF -DOP1_HOST_SIM $(REF_INC) cs_hall_compare.cc $(REF_SRC) -o $@

$(BUILD)/hall_coop: cs_hall_compare.cc $(COOP_SRC) $(wildcard $(COOP)/*.h)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -I$(COOP) -I../../../sdk cs_hall_compare.cc $(COOP_SRC) -o $@

$(BUILD)/hall_coop.elf: cs_hall_compare.cc $(COOP_SRC) $(wildcard $(COOP)/*.h $(COOP)/*.S)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -I$(COOP) -I../../../sdk cs_hall_compare.cc $(COOP_SRC) \
	  $(COOP)/hall_bfin.S $(COOP)/halfband_bfin.S -o $@

clean:
	rm -rf $(BUILD)
