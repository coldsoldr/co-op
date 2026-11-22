# The idle paths' host test (fx_idle_test.cc; docs/efficiency-audit.md finding 6): each effect at two positions in
# lockstep, one as it ships, one with its idle path switched off, every output sample compared; and the render's
# cycles playing, silent and idle in the Blackfin GNU simulator.
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/idle'              host, all five
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/idle sim'          simulator cycles
#   ... make -C tests/host/idle FX=tape_echo       one effect (tape_echo cs_hall cs_plate fickle petri)
#   IDLE_TRACE=1 IDLE_ONLY=n build/host/idle/idle_<fx>   one setting of the note-and-silence test, its tail every 2.9 s
HOST_CXX ?= g++
P := ../../../plugins
BUILD ?= ../../../build/host/idle
BLOCKS ?= 20000
FX ?= tape_echo cs_hall cs_plate fickle petri
BFIN ?= $(HOME)/op1-toolchain/bfin/bin

ID_tape_echo := 1
ID_cs_hall := 2
ID_cs_plate := 3
ID_fickle := 4
ID_petri := 5
SRC_tape_echo := tape_echo.cc tape_echo_screen.cc src/tape_echo_core.cc src/halfband.cc
ASM_tape_echo := src/tape_echo_bfin.S ../shared/halfband_bfin.S
SRC_cs_hall := cs_hall.cc cs_hall_screen.cc hall.cc halfband.cc
ASM_cs_hall := hall_bfin.S ../shared/halfband_bfin.S
SRC_cs_plate := cs_plate.cc cs_plate_screen.cc plate.cc halfband.cc
ASM_cs_plate := plate_bfin.S ../shared/halfband_bfin.S
SRC_fickle := fickle.cc fickle_screen.cc
ASM_fickle := fickle_bfin.S
SRC_petri := petri.cc petri_screen.cc src/petri_engine.cc src/halfband.cc
ASM_petri := ../shared/halfband_bfin.S src/voice_bfin.S src/space_bfin.S
# the simulator builds: the plugins' own clocks stepped by the test where they have the switch
SIMDEF_tape_echo := -DCOOP_TAPE_ECHO_HOST_CLOCK
SIMDEF_petri := -DCOOP_PETRI_HOST_CLOCK

scratch = $(shell python3 -c "import json; print(json.load(open('$(P)/$(1)/plugin.json'))['scratch'])")
COMMON := ../fx_scratch_host.cc ../screen_host.cc ../tempo_host.cc
INC = -I$(P)/$(1) -I../../../sdk -I../../../core/include
DEFS = -DFXSYM=FX_$(1) -DFXID=$(ID_$(1)) -DCOOP_FX_SCRATCH_BYTES=$(call scratch,$(1))
# the host lockstep reads the plugins' probe; the simulator measures them as they ship (an idle block shows in its cost)
PROBE := -DCOOP_IDLE_PROBE
CXXFLAGS := -O2 -std=c++17 -Wall -Wno-unused-function -Wno-unused-variable
SIMFLAGS := -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -fno-tree-loop-distribute-patterns

.PHONY: all sim clean $(addprefix run-,$(FX)) $(addprefix sim-,$(FX))
all: $(addprefix run-,$(FX))
sim: $(addprefix sim-,$(FX))

define RULES
$$(BUILD)/idle_$(1): fx_idle_test.cc $$(wildcard $$(P)/$(1)/*.cc $$(P)/$(1)/*.h $$(P)/$(1)/src/*) $$(COMMON)
	@mkdir -p $$(BUILD)
	$$(HOST_CXX) $$(CXXFLAGS) $$(PROBE) $$(call DEFS,$(1)) $$(call INC,$(1)) fx_idle_test.cc $$(addprefix $$(P)/$(1)/,$$(SRC_$(1))) $$(COMMON) -o $$@

run-$(1): $$(BUILD)/idle_$(1)
	$$(BUILD)/idle_$(1) $$(BLOCKS)

$$(BUILD)/idle_$(1).elf: fx_idle_test.cc $$(wildcard $$(P)/$(1)/*.cc $$(P)/$(1)/*.h $$(P)/$(1)/*.S $$(P)/$(1)/src/*) $$(P)/shared/halfband_bfin.S $$(COMMON)
	@mkdir -p $$(BUILD)
	$$(BFIN)/bfin-elf-g++ $$(SIMFLAGS) $$(call DEFS,$(1)) $$(SIMDEF_$(1)) $$(call INC,$(1)) fx_idle_test.cc \
	  $$(addprefix $$(P)/$(1)/,$$(SRC_$(1)) $$(ASM_$(1))) $$(COMMON) -o $$@

sim-$(1): $$(BUILD)/idle_$(1).elf
	$$(BFIN)/bfin-elf-run $$(BUILD)/idle_$(1).elf
endef
$(foreach f,tape_echo cs_hall cs_plate fickle petri,$(eval $(call RULES,$(f))))

clean:
	rm -rf $(BUILD)
