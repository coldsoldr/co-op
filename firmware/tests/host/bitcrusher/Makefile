# The bitcrusher port's host test (bitcrusher_compare.cc): plugins/bitcrusher against te-op1-cfw's crush core and
# its kennel wrapper's parameter mapping, bit for bit.
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/bitcrusher'
#   ... make -C tests/host/bitcrusher profile      (the render's compute in the GNU simulator, bfin-elf-run)
HOST_CXX ?= g++
TE ?= ../../../../../te-op1-cfw/ports
COOP := ../../../plugins/bitcrusher
BUILD ?= ../../../build/host/bitcrusher
INC := -I$(COOP) -I../../../sdk -I$(TE)/knl -I$(TE) -I../../../third_party/kennel/synth_sdk
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -Wno-unused-function -Wno-int-to-pointer-cast
SRCS := bitcrusher_compare.cc $(COOP)/bitcrusher.cc
BFIN ?= $(HOME)/op1-toolchain/bfin/bin
SIMFLAGS := -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -Wno-unused-function -DBC_PROFILE

.PHONY: all profile clean
all: $(BUILD)/bitcrusher_compare
	$(BUILD)/bitcrusher_compare
	BC_LOW_BITS=1 $(BUILD)/bitcrusher_compare

$(BUILD)/bitcrusher_compare: $(SRCS) $(COOP)/src/crush.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) $(INC) $(SRCS) -o $@

profile: $(SRCS) $(COOP)/src/crush.h
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) $(INC) $(SRCS) -o $(BUILD)/bitcrusher_sim
	$(BFIN)/bfin-elf-run $(BUILD)/bitcrusher_sim 4000

clean:
	rm -rf $(BUILD)
