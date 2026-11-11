# The cs_plate port's host test (cs_plate_compare.cc): plugins/cs_plate against te-op1-cfw's plate and its kennel
# glue's half-rate path, bit for bit; on the host (C kernels both sides) and in the GNU Blackfin simulator (the
# plugin's assembly against the upstream's C).
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/cs_plate'
#   (from a worktree, pass TE=/mnt/i/GitHub/coldsoldr/te-op1-cfw/ports)
# The "conflicting C language linkage" warnings are expected: both sides declare the same assembly entry points in
# their own namespaces. In the simulator build the upstream (OP1FX_NO_ASM) never calls them.
HOST_CXX ?= g++
TE ?= ../../../../../te-op1-cfw/ports
COOP := ../../../plugins/cs_plate
BUILD ?= ../../../build/host/cs_plate
SEGMENTS ?= 40
SIM_SEGMENTS ?= 6
BFIN ?= $(HOME)/op1-toolchain/bfin/bin

INC := -I$(COOP) -I../../../sdk -I$(TE) -I$(TE)/fx -I$(TE)/clouds -I$(TE)/rings
WARN := -Wall -Wno-unused-variable -Wno-unused-function -Wno-class-memaccess
# e8: the position buffers (../fx_scratch_host.cc), sized from plugin.json "scratch"
SCRATCH := $(shell python3 -c "import json; print(json.load(open('$(COOP)/plugin.json'))['scratch'])")
HOST_FLAGS := -DCOOP_FX_SCRATCH_BYTES=$(SCRATCH) -O2 -std=c++17 $(WARN) -DOP1_HOST_SIM
SIM_FLAGS := -DCOOP_FX_SCRATCH_BYTES=$(SCRATCH) -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -fno-tree-loop-distribute-patterns $(WARN) \
             -DOP1_HOST_SIM -DOP1FX_NO_ASM -DOP1CLOUDS_NO_ASM
COOP_SRC := $(COOP)/cs_plate.cc $(COOP)/cs_plate_screen.cc $(COOP)/plate.cc $(COOP)/halfband.cc ../fx_scratch_host.cc \
            ../screen_host.cc
COOP_ASM := $(COOP)/plate_bfin.S $(COOP)/halfband_bfin.S
REF_SRC := $(TE)/fx/src/plate/plate.cc $(TE)/clouds/src/core/halfband.cc $(TE)/clouds/src/core/clouds_tables.cc \
           $(TE)/rings/src/fixed_tables.cc

.PHONY: all host sim clean
all: host sim

host: $(BUILD)/cs_plate_compare
	$(BUILD)/cs_plate_compare $(SEGMENTS)

sim: $(BUILD)/cs_plate_compare.elf
	$(BFIN)/bfin-elf-run $(BUILD)/cs_plate_compare.elf $(SIM_SEGMENTS)

$(BUILD)/cs_plate_compare: cs_plate_compare.cc $(COOP_SRC) $(wildcard $(COOP)/*.h) ../../../sdk/coop_fx_screen.h ../../../sdk/coop_screen.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) $(INC) cs_plate_compare.cc $(COOP_SRC) $(REF_SRC) -o $@

$(BUILD)/cs_plate_compare.elf: cs_plate_compare.cc $(COOP_SRC) $(COOP_ASM) $(wildcard $(COOP)/*.h) ../../../sdk/coop_fx_screen.h ../../../sdk/coop_screen.h
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIM_FLAGS) $(INC) cs_plate_compare.cc $(COOP_SRC) $(COOP_ASM) $(REF_SRC) -o $@

clean:
	rm -rf $(BUILD)
