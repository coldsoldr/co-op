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
# e10: the tempos (tenths of a BPM) the host run covers: 120 (the scenario as before the export), 90, 133.5, 174, and 0
# = a walk through seven tempos during the run (tape_echo_compare.cc kWalk). The simulator runs SIM_TEMPO.
TEMPOS ?= 1200 900 1335 1740 0
SIM_TEMPO ?= 1335
# e8: the position buffers (../fx_scratch_host.cc), sized from plugin.json "scratch"
SCRATCH := $(shell python3 -c "import json; print(json.load(open('$(COOP)/plugin.json'))['scratch'])")
CXXFLAGS := -DCOOP_FX_SCRATCH_BYTES=$(SCRATCH) -O2 -std=c++17 -Wall -Wno-unused-function
REF_INC := -I$(TE)/fx -I$(TE) -I$(TE)/rings -I$(TE)/clouds
REF_SRC := $(TE)/fx/src/tape_echo/tape_echo.cc $(TE)/clouds/src/core/halfband.cc $(TE)/clouds/src/core/clouds_tables.cc \
           $(TE)/rings/src/fixed_tables.cc
# e9: the screen file compiles and links with the render it shares state with (../screen_host.cc: the core's screen
# calls as stubs); the test compares audio only
COOP_SRC := $(COOP)/tape_echo.cc $(COOP)/tape_echo_screen.cc $(COOP)/src/tape_echo_core.cc $(COOP)/src/halfband.cc \
            ../fx_scratch_host.cc ../screen_host.cc ../tempo_host.cc
COOP_DEPS := $(wildcard $(COOP)/*.cc $(COOP)/*.h $(COOP)/src/*) ../../../sdk/coop_fx_screen.h ../../../sdk/coop_screen.h \
             ../../../sdk/coop_plugin.h ../../../core/include/coop_tempo.h ../tempo_host.cc

BFIN ?= $(HOME)/op1-toolchain/bfin/bin
SIMFLAGS := -DCOOP_FX_SCRATCH_BYTES=$(SCRATCH) -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -fno-tree-loop-distribute-patterns

.PHONY: all sim tables clean
all: $(BUILD)/tape_echo_ref $(BUILD)/tape_echo_coop
	@set -e; for t in $(TEMPOS); do \
	  echo "== tempo $$t (tenths of a BPM; 0 = the walk)"; \
	  $(BUILD)/tape_echo_ref $(BUILD)/ref.raw $(BLOCKS) $$t > $(BUILD)/ref.txt; cat $(BUILD)/ref.txt; \
	  $(BUILD)/tape_echo_coop $(BUILD)/coop.raw $(BLOCKS) $$t > $(BUILD)/coop.txt; cat $(BUILD)/coop.txt; \
	  cmp $(BUILD)/ref.txt $(BUILD)/coop.txt; echo "tape_echo: the sync divisions' time words agree (tempo $$t)"; \
	  cmp $(BUILD)/ref.raw $(BUILD)/coop.raw; echo "tape_echo: coop == te-op1-cfw, bit-identical ($(BLOCKS) blocks, host, tempo $$t)"; \
	done

$(BUILD)/tape_echo_ref: tape_echo_compare.cc
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -Wno-class-memaccess -DTE_REF -DOP1_HOST_SIM $(REF_INC) -I../../../core/include tape_echo_compare.cc $(REF_SRC) -o $@

$(BUILD)/tape_echo_coop: tape_echo_compare.cc $(COOP_DEPS)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -I$(COOP) -I../../../sdk -I../../../core/include tape_echo_compare.cc $(COOP_SRC) -o $@

# The simulator: coop as it ships (its .S kernels; the clock stepped by the test), the original in C.
$(BUILD)/tape_echo_ref.elf: tape_echo_compare.cc
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -Wno-class-memaccess -DTE_REF -DOP1_HOST_SIM -DOP1FX_NO_ASM -DOP1CLOUDS_NO_ASM \
	  $(REF_INC) -I../../../core/include tape_echo_compare.cc $(REF_SRC) $(TE)/fx/src/tape_echo/tape_echo_bfin.S $(TE)/clouds/src/core/halfband_bfin.S -o $@

$(BUILD)/tape_echo_coop.elf: tape_echo_compare.cc $(COOP_DEPS)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -DCOOP_TAPE_ECHO_HOST_CLOCK -I$(COOP) -I../../../sdk -I../../../core/include tape_echo_compare.cc $(COOP_SRC) \
	  $(COOP)/src/tape_echo_bfin.S $(COOP)/src/halfband_bfin.S -o $@

sim: $(BUILD)/tape_echo_ref.elf $(BUILD)/tape_echo_coop.elf
	$(BFIN)/bfin-elf-run $(BUILD)/tape_echo_ref.elf $(BUILD)/ref_sim.raw $(SIM_BLOCKS) $(SIM_TEMPO)
	$(BFIN)/bfin-elf-run $(BUILD)/tape_echo_coop.elf $(BUILD)/coop_sim.raw $(SIM_BLOCKS) $(SIM_TEMPO)
	cmp $(BUILD)/ref_sim.raw $(BUILD)/coop_sim.raw && echo "tape_echo: coop (asm) == te-op1-cfw (C), bit-identical ($(SIM_BLOCKS) blocks, simulator, tempo $(SIM_TEMPO))"

tables: gen_tables.cc
	@mkdir -p $(BUILD)
	$(HOST_CXX) -O2 -std=c++17 -Wno-unused-function -I$(TE)/fx -I$(TE) -I$(TE)/clouds -I$(TE)/rings gen_tables.cc \
	  $(TE)/clouds/src/core/halfband.cc $(TE)/rings/src/fixed_tables.cc -o $(BUILD)/gen_tables
	$(BUILD)/gen_tables > $(COOP)/src/tape_echo_tables.h

clean:
	rm -rf $(BUILD)
