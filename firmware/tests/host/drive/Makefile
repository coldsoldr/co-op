# The drive port's tests (drive_compare.cc, tables_check.cc): plugins/drive against te-op1-cfw's ports/fx/src/drive
# behind kennel's stereo glue, bit for bit.
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/drive all sim'
#   all     host: the generated tables against upstream's constexpr ones; the plugin (C passes) against upstream (C)
#   sim     GNU simulator: the plugin as the device builds it (assembly passes, -DOP1FX_DRIVE_ASM_ONLY) against
#           upstream's C passes, and the plugin's render cycles per block
# TE: te-op1-cfw's ports/fx (from a worktree, pass TE=/mnt/i/GitHub/coldsoldr/te-op1-cfw/ports/fx).
HOST_CXX ?= g++
TE ?= ../../../../../te-op1-cfw/ports/fx
COOP := ../../../plugins/drive
SDK := ../../../sdk
BUILD ?= ../../../build/host/drive
CXXFLAGS := -O2 -std=c++17 -Wall -Wno-unused-function -Wno-class-memaccess
# upstream beside the plugin's copy: its namespace renamed, and its assembly entry points (unused by the reference,
# which runs the C passes) too, so they don't collide with the plugin's
ASM_NAMES := DriveInterpAsm DriveShapeAsm DriveDecimAsm DriveToneAsm DriveDcAsm DriveMixAsm DriveMonoWetAsm
REF_DEFS := -Dop1fx=up $(foreach s,$(ASM_NAMES),-D$(s)=Ref$(s) -D_$(s)=_Ref$(s))
COOP_SRC := $(COOP)/drive.cc $(COOP)/src/drive_dsp.cc

BFIN ?= $(HOME)/op1-toolchain/bfin/bin
SIMFLAGS := -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -Wno-class-memaccess

.PHONY: all tables host sim clean
all: tables host

tables: $(BUILD)/tables_check
	$(BUILD)/tables_check

host: $(BUILD)/drive_compare
	$(BUILD)/drive_compare

$(BUILD)/tables_check: tables_check.cc $(COOP)/src/drive_tables.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -Wno-unused-variable -Dop1fx=up -I$(TE) '-DCOOP_TABLES="$(abspath $(COOP))/src/drive_tables.h"' \
	  tables_check.cc -o $@

$(BUILD)/drive_compare: drive_compare.cc drive_ref.cc $(COOP_SRC) $(wildcard $(COOP)/src/*.h)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) $(REF_DEFS) -I$(TE) -c drive_ref.cc -o $(BUILD)/ref.o
	$(HOST_CXX) $(CXXFLAGS) $(REF_DEFS) -I$(TE) -c $(TE)/src/drive/drive.cc -o $(BUILD)/ref_dsp.o
	$(HOST_CXX) $(CXXFLAGS) -I$(COOP) -I$(SDK) drive_compare.cc $(COOP_SRC) $(BUILD)/ref.o $(BUILD)/ref_dsp.o -o $@

sim: drive_compare.cc drive_ref.cc $(COOP_SRC) $(COOP)/src/drive_bfin.S
	@mkdir -p $(BUILD)/sim
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) $(REF_DEFS) -I$(TE) -c drive_ref.cc -o $(BUILD)/sim/ref.o
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) $(REF_DEFS) -I$(TE) -c $(TE)/src/drive/drive.cc -o $(BUILD)/sim/ref_dsp.o
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) $(REF_DEFS) -c $(TE)/src/drive/drive_bfin.S -o $(BUILD)/sim/ref_asm.o
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -DOP1FX_DRIVE_ASM_ONLY -I$(COOP) -I$(SDK) drive_compare.cc $(COOP_SRC) \
	  $(COOP)/src/drive_bfin.S $(BUILD)/sim/ref.o $(BUILD)/sim/ref_dsp.o $(BUILD)/sim/ref_asm.o -o $(BUILD)/sim/drive_compare
	$(BFIN)/bfin-elf-run $(BUILD)/sim/drive_compare

clean:
	rm -rf $(BUILD)
