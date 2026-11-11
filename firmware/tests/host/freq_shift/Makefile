# The freq_shift port's host test (freq_shift_compare.cc): plugins/freq_shift against te-op1-cfw's ports/fx/src/shift.
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/freq_shift'
HOST_CXX ?= g++
TE ?= ../../../../../te-op1-cfw/ports
COOP := ../../../plugins/freq_shift
BUILD ?= ../../../build/host/freq_shift
# e8: the position buffers (../fx_scratch_host.cc), sized from plugin.json "scratch"
SCRATCH := $(shell python3 -c "import json; print(json.load(open('$(COOP)/plugin.json'))['scratch'])")
CXXFLAGS := -DCOOP_FX_SCRATCH_BYTES=$(SCRATCH) -O2 -std=c++17 -Wall -Wextra
BFIN ?= $(HOME)/op1-toolchain/bfin/bin
SIMFLAGS := -DCOOP_FX_SCRATCH_BYTES=$(SCRATCH) -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti

.PHONY: all clean sim
all: $(BUILD)/fs_ref $(BUILD)/fs_ref_st $(BUILD)/fs_coop
	$(BUILD)/fs_ref $(BUILD)/ref0.raw 0
	$(BUILD)/fs_coop $(BUILD)/coop0.raw 0
	cmp $(BUILD)/ref0.raw $(BUILD)/coop0.raw && echo "freq_shift, mixed L == R / L != R: coop == te-op1-cfw (a pair a channel), bit-identical"
	$(BUILD)/fs_ref_st $(BUILD)/refst1.raw 1
	$(BUILD)/fs_coop $(BUILD)/coop1.raw 1
	cmp $(BUILD)/refst1.raw $(BUILD)/coop1.raw && echo "freq_shift, L == R: coop == te-op1-cfw's stereo path (the kennel master), bit-identical"

# te-op1-cfw: the DSP (C kernels) and the kennel glue's gain map (common/knl_fx.h, which includes kennel's op1_plugin.h).
$(BUILD)/fs_ref: freq_shift_compare.cc $(TE)/fx/src/shift/shift.cc
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DFS_REF -I$(TE)/fx -I$(TE)/knl -I$(TE)/../third_party/kennel/synth_sdk -Wno-int-to-pointer-cast $^ -o $@

$(BUILD)/fs_ref_st: freq_shift_compare.cc $(TE)/fx/src/shift/shift.cc
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DFS_REF_ST -I$(TE)/fx -I$(TE)/knl -I$(TE)/../third_party/kennel/synth_sdk -Wno-int-to-pointer-cast $^ -o $@

$(BUILD)/fs_coop: freq_shift_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/*.h $(COOP)/src/*) ../screen_host.cc
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DFREQ_SHIFT_TEST -I$(COOP) -I../../../sdk freq_shift_compare.cc $(COOP)/freq_shift.cc \
	  $(COOP)/freq_shift_screen.cc $(COOP)/src/shift.cc ../fx_scratch_host.cc ../screen_host.cc -o $@

# The plugin's assembly path in the GNU simulator against its C kernels (-DSHIFT_C_KERNELS), the same scenarios:
# make sim. Also prints the cycles of a render (no cache or SDRAM model).
$(BUILD)/fs_sim.elf: freq_shift_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/*.h $(COOP)/src/*) ../screen_host.cc
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -DFREQ_SHIFT_TEST -DFS_SIM -DSHIFT_C_KERNELS -I$(COOP) -I../../../sdk \
	  freq_shift_compare.cc $(COOP)/freq_shift.cc $(COOP)/freq_shift_screen.cc $(COOP)/src/shift.cc \
	  $(COOP)/src/shift_bfin.S ../fx_scratch_host.cc ../screen_host.cc -o $@

sim: $(BUILD)/fs_sim.elf $(BUILD)/fs_coop
	$(BUILD)/fs_coop $(BUILD)/coop0.raw 0 > /dev/null
	$(BFIN)/bfin-elf-run $< $(BUILD)/sim0.raw 0
	cmp $(BUILD)/coop0.raw $(BUILD)/sim0.raw && echo "freq_shift: Blackfin assembly == host C, bit-identical"
	$(BFIN)/bfin-elf-run $< $(BUILD)/simc0.raw 0 c
	cmp $(BUILD)/coop0.raw $(BUILD)/simc0.raw && echo "freq_shift: Blackfin C kernels == host C, bit-identical"

clean:
	rm -rf $(BUILD)
