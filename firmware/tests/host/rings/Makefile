# The rings port's host test (rings_compare.cc): plugins/rings against te-op1-cfw's ports/rings, bit for bit.
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/rings'
HOST_CXX ?= g++
TE ?= ../../../../../te-op1-cfw/ports/rings
COOP := ../../../plugins/rings
BUILD ?= ../../../build/host/rings
RATE := -DOP1RINGS_SAMPLE_RATE=22050 -DOP1RINGS_CONTROL_BLOCK_SIZE=32 -DOP1RINGS_UPSAMPLE=4
CXXFLAGS := -O2 -std=c++17 -Wall -Wno-unused-variable -Wno-unused-function -Wno-class-memaccess $(RATE)
DSP := src/op1_voice.cc src/string_voice.cc src/string_fixed.cc src/reverb_fixed.cc src/fm_voice_fixed.cc \
       src/modal_resonator.cc src/fixed_tables.cc

BFIN ?= $(HOME)/op1-toolchain/bfin/bin
SIMFLAGS := -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -fno-tree-loop-distribute-patterns \
            -Wno-class-memaccess $(RATE)
PROFILE_BLOCKS ?= 62010
PROFILE_MODEL ?=

.PHONY: voice44 all clean profile halfband-check halved batched footprint dirty idle idle-sim models sleep sleep-sim early early-bins \
        early-table early-sim
all: $(BUILD)/rings_ref $(BUILD)/rings_coop $(BUILD)/rings_coop_m14
	$(BUILD)/rings_ref $(BUILD)/ref.raw
	$(BUILD)/rings_coop $(BUILD)/coop.raw
	cmp $(BUILD)/ref.raw $(BUILD)/coop.raw && echo "rings: coop == te-op1-cfw, bit-identical"
	RINGS_NO_LEASE=1 $(BUILD)/rings_coop $(BUILD)/coop_nolease.raw
	cmp $(BUILD)/ref.raw $(BUILD)/coop_nolease.raw && echo "rings, every lease refused: bit-identical"
	$(BUILD)/rings_coop_m14 $(BUILD)/coop_m14.raw
	cmp $(BUILD)/ref.raw $(BUILD)/coop_m14.raw && echo "rings, 14 modes a voice in L1 (moves across it): bit-identical"

# rings 0.4's L1 home for the modes holds 10 (the shipped polyphony 4); this build's "all" plays 14, so with 14 in L1
# the modes move between L1 and the voice state on every polyphony change.
$(BUILD)/rings_coop_m14: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DOP1_COOP -DOP1RINGS_L1_MODES=14 -I$(COOP) -I../../../sdk rings_compare.cc $(COOP)/rings.cc \
	  $(addprefix $(COOP)/,$(DSP)) -o $@

# te-op1-cfw's own engine (the firmware build's engine.cc in its simulator mode: its screen, input and patch table
# only have to link).
$(BUILD)/rings_ref: rings_compare.cc
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DRINGS_REF -DOP1_SIM -DOP1_HOST_SIM -I$(TE) rings_compare.cc $(TE)/op1/engine.cc \
	  $(TE)/op1/screen.cc $(TE)/op1/input.cc $(TE)/op1/patches.cc $(addprefix $(TE)/,$(DSP)) -o $@

$(BUILD)/rings_coop: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DOP1_COOP -I$(COOP) -I../../../sdk rings_compare.cc $(COOP)/rings.cc \
	  $(addprefix $(COOP)/,$(DSP)) -o $@

# Compute cycles per call in the GNU simulator (no cache or SDRAM model): make profile [PROFILE_BLOCKS=n]
$(BUILD)/rings_profile.elf: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -DOP1_COOP -DRINGS_PROFILE $(PROFILE_FLAGS) -I$(COOP) -I../../../sdk rings_compare.cc \
	  $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) $(ASM) -o $@

profile: $(BUILD)/rings_profile.elf
	$(BFIN)/bfin-elf-run $< $(BUILD)/profile.raw $(PROFILE_BLOCKS) $(PROFILE_MODEL)

clean:
	rm -rf $(BUILD)

# The upsampler's half-band kernel against its C loop, in the simulator.
$(BUILD)/halfband_check.elf: halfband_check.cc $(COOP)/src/halfband_bfin.S $(COOP)/src/halfband_taps.h
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -I$(COOP) halfband_check.cc $(COOP)/src/halfband_bfin.S -o $@

halfband-check: $(BUILD)/halfband_check.elf
	$(BFIN)/bfin-elf-run $<

# The coop build as it ships (OP1RINGS_CONTROL_HALVED=1, plugin.json): close to te-op1-cfw, not bit-identical.
$(BUILD)/rings_halved: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DOP1_COOP -DOP1RINGS_CONTROL_HALVED=1 -I$(COOP) -I../../../sdk rings_compare.cc \
	  $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) -o $@

halved: $(BUILD)/rings_halved $(BUILD)/rings_ref
	$(BUILD)/rings_ref $(BUILD)/ref.raw
	$(BUILD)/rings_halved $(BUILD)/halved.raw
	python3 -I compare_raw.py $(BUILD)/ref.raw $(BUILD)/halved.raw
	python3 -I compare_spectra.py $(BUILD)/ref.raw.mix $(BUILD)/halved.raw.mix

# The batched control work (OP1RINGS_CONTROL_BATCHED) against the halved schedule alone, and the build as it ships
# (plugin.json: halved, batched, POLY4). `make batched` compares the first two in a steady state (no knob jumps or
# bends: the batch uses each voice's last block, so only a moving control lands differently).
$(BUILD)/rings_batched: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DOP1_COOP -DOP1RINGS_CONTROL_HALVED=1 -DOP1RINGS_CONTROL_BATCHED=1 -I$(COOP) \
	  -I../../../sdk rings_compare.cc $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) -o $@

$(BUILD)/rings_shipped: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DOP1_COOP -DOP1RINGS_CONTROL_HALVED=1 -DOP1RINGS_CONTROL_BATCHED=1 -DOP1RINGS_COOP_POLY4=1 \
	  -I$(COOP) -I../../../sdk rings_compare.cc $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) -o $@

# 0.4's 44.1 kHz voice (OP1RINGS_VOICE_44K, plugin.json): its 64 samples a render, repeated as the core does, must be
# the even samples of the 88.2 kHz build, bit for bit (the odd ones were the Lagrange stage's). `make voice44`.
$(BUILD)/rings_shipped44: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DOP1_COOP -DOP1RINGS_CONTROL_HALVED=1 -DOP1RINGS_CONTROL_BATCHED=1 -DOP1RINGS_COOP_POLY4=1 	  -DOP1RINGS_VOICE_44K=1 -I$(COOP) -I../../../sdk rings_compare.cc $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) -o $@

voice44: $(BUILD)/rings_shipped $(BUILD)/rings_shipped44
	$(BUILD)/rings_shipped $(BUILD)/shipped.raw > /dev/null
	$(BUILD)/rings_shipped44 $(BUILD)/shipped44.raw > /dev/null
	python3 -I even_check.py $(BUILD)/shipped.raw $(BUILD)/shipped44.raw

batched: $(BUILD)/rings_halved $(BUILD)/rings_batched $(BUILD)/rings_shipped
	@for m in 0 1 2 3 4 5; do \
	  RINGS_NO_JUMP=1 RINGS_NO_BEND=1 $(BUILD)/rings_halved $(BUILD)/h$$m.raw 8000 $$m 3 > /dev/null; \
	  RINGS_NO_JUMP=1 RINGS_NO_BEND=1 $(BUILD)/rings_batched $(BUILD)/b$$m.raw 8000 $$m 3 > /dev/null; \
	  printf "model $$m, polyphony 4: "; python3 -I block_levels.py $(BUILD)/h$$m.raw $(BUILD)/b$$m.raw; done
	$(BUILD)/rings_shipped $(BUILD)/shipped.raw

# Rings' instruction working set per block (code_footprint.py), the shipped build in the simulator with an
# instruction trace: make footprint [FOOTPRINT_MODEL=m] (blocks 120..159 of the scenario at polyphony setting 0).
SHIPPED_FLAGS := -DOP1RINGS_CONTROL_HALVED=1 -DOP1RINGS_CONTROL_BATCHED=1 -DOP1RINGS_COOP_POLY4=1
FOOTPRINT_MODEL ?= 0
ASM := $(COOP)/src/mode_bank_bfin.S $(COOP)/src/string_kernel_bfin.S $(COOP)/src/limiter_bfin.S \
       $(COOP)/src/reverb_bfin.S $(COOP)/src/halfband_bfin.S $(wildcard $(COOP)/src/fm_voice_bfin.S $(COOP)/src/upsampler_bfin.S $(COOP)/src/modal_bfin.S)
$(BUILD)/rings_trace.elf: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -DOP1_COOP -DRINGS_TRACE $(SHIPPED_FLAGS) $(FOOTPRINT_FLAGS) -I$(COOP) -I../../../sdk \
	  rings_compare.cc $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) $(ASM) -o $@

footprint: $(BUILD)/rings_trace.elf
	$(BFIN)/bfin-elf-run --trace-insn=on $< $(BUILD)/trace.raw 160 $(FOOTPRINT_MODEL) 0 2>&1 | \
	  python3 -I code_footprint.py $< $(BFIN)/bfin-elf-nm 120 160

# Rings' store footprint per block (dirty_lines.py): the shipped build in the simulator, its .bss compared block to
# block in 32-byte lines (rings_compare.cc -DRINGS_DIRTY): make dirty [DIRTY_MODEL=m] [DIRTY_BLOCKS=n]
DIRTY_MODEL ?= 0
DIRTY_BLOCKS ?= 400
$(BUILD)/rings_dirty.elf: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -g -DOP1_COOP -DRINGS_DIRTY $(SHIPPED_FLAGS) $(DIRTY_FLAGS) -I$(COOP) -I../../../sdk \
	  rings_compare.cc $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) $(ASM) -o $@

dirty: $(BUILD)/rings_dirty.elf
	$(BFIN)/bfin-elf-run $< $(BUILD)/dirty.raw $(DIRTY_BLOCKS) $(DIRTY_MODEL) 0 | \
	  python3 -I dirty_lines.py $< $(BFIN)

# 0.11: the shared reverb's rounding and its idle state (rings_idle.cc), the string + reverb model with keys held.
#   make idle                      the build as it ships against its twin that never idles: every sample the same
#   make idle OLD=<0.10's plugin folder>   and against 0.10: the change in the sound
#   make idle IDLE_DAMPING=32767 IDLE_HOLD=240    the longest decay, the chord held four minutes
#   make idle-sim                  cycles a block before the idle state and in it (the simulator)
#   make models OLD=<0.10's plugin folder>   the other five models against 0.10: byte for byte
IDLE_DAMPING ?= 24000
IDLE_HOLD ?= 24
IDLE_FLAGS := -DOP1_COOP -DCOOP_IDLE_PROBE -DOP1RINGS_CONTROL_HALVED=1 -DOP1RINGS_CONTROL_BATCHED=1 \
              -DOP1RINGS_COOP_POLY4=1 -DOP1RINGS_COOP_POLY_ALL=1 -DOP1RINGS_VOICE_44K=1
IDLE_DEPS := rings_idle.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
$(BUILD)/rings_idle: $(IDLE_DEPS)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) $(IDLE_FLAGS) -I$(COOP) -I../../../sdk rings_idle.cc $(COOP)/rings.cc \
	  $(addprefix $(COOP)/,$(DSP)) -o $@

$(BUILD)/rings_idle_twin: $(IDLE_DEPS)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) $(IDLE_FLAGS) -DOP1RINGS_NO_REVERB_IDLE -I$(COOP) -I../../../sdk rings_idle.cc \
	  $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) -o $@

IDLE_BLOCK = `sed -n 's/.*IDLE_BLOCK \([0-9]*\).*/\1/p' $(BUILD)/idle.txt`
idle: $(BUILD)/rings_idle $(BUILD)/rings_idle_twin
	$(BUILD)/rings_idle run $(BUILD)/idle.raw $(IDLE_DAMPING) $(IDLE_HOLD) | tee $(BUILD)/idle.txt
	$(BUILD)/rings_idle_twin run $(BUILD)/idle_twin.raw $(IDLE_DAMPING) $(IDLE_HOLD)
	@echo "as it ships / its twin that never idles:"
	$(BUILD)/rings_idle twin $(BUILD)/idle.raw $(BUILD)/idle_twin.raw $(IDLE_BLOCK) $(IDLE_HOLD)
	$(BUILD)/rings_idle reverb
ifneq ($(OLD),)
	$(HOST_CXX) $(CXXFLAGS) $(IDLE_FLAGS) -I$(OLD) -I../../../sdk rings_idle.cc $(OLD)/rings.cc \
	  $(addprefix $(OLD)/,$(DSP)) -o $(BUILD)/rings_idle_old
	$(BUILD)/rings_idle_old run $(BUILD)/idle_old.raw $(IDLE_DAMPING) $(IDLE_HOLD)
	@echo "as it ships / $(OLD):"
	$(BUILD)/rings_idle cmp $(BUILD)/idle.raw $(BUILD)/idle_old.raw $(IDLE_BLOCK) $(IDLE_HOLD)
	$(BUILD)/rings_idle_old reverb
endif

$(BUILD)/rings_idle.elf: $(IDLE_DEPS)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) $(IDLE_FLAGS) -I$(COOP) -I../../../sdk rings_idle.cc $(COOP)/rings.cc \
	  $(addprefix $(COOP)/,$(DSP)) $(ASM) -o $@

$(BUILD)/rings_idle_twin.elf: $(IDLE_DEPS)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) $(IDLE_FLAGS) -DOP1RINGS_NO_REVERB_IDLE -I$(COOP) -I../../../sdk rings_idle.cc \
	  $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) $(ASM) -o $@

idle-sim: $(BUILD)/rings_idle.elf $(BUILD)/rings_idle_twin.elf
	$(BFIN)/bfin-elf-run $(BUILD)/rings_idle.elf
	$(BFIN)/bfin-elf-run $(BUILD)/rings_idle_twin.elf

# The models the reverb has no part in (modal, sympathetic, string, FM, quantised) against an older version's sources:
# rings_compare.cc's scenario, 8,000 blocks a model at polyphony "all" and 4, the two outputs compared byte for byte.
models: $(BUILD)/rings_coop
ifeq ($(OLD),)
	@echo "make models OLD=<the older version's plugin folder>"; exit 2
endif
	$(HOST_CXX) $(CXXFLAGS) -DOP1_COOP -I$(OLD) -I../../../sdk rings_compare.cc $(OLD)/rings.cc \
	  $(addprefix $(OLD)/,$(DSP)) -o $(BUILD)/rings_coop_old
	@set -e; for m in 0 1 2 3 4; do for q in 0 3; do \
	  $(BUILD)/rings_coop $(BUILD)/m_new.raw 8000 $$m $$q > /dev/null 2>&1; \
	  $(BUILD)/rings_coop_old $(BUILD)/m_old.raw 8000 $$m $$q > /dev/null 2>&1; \
	  cmp $(BUILD)/m_new.raw $(BUILD)/m_old.raw; echo "model $$m, polyphony setting $$q: the same bytes"; done; done

# rings 0.12: a held voice whose tail stays under 4 LSB for 10 s sleeps (src/op1_voice.h kQuietLevel; rings_sleep.cc).
#   make sleep                     six keys held, every model: as it ships against its twin with 0.11's rule alone
#                                  (-DOP1RINGS_NO_QUIET_SLEEP): the default patch (the same samples), then settings
#                                  whose tails stall (the difference, and when each voice sleeps)
#   make sleep OLD=<0.11's plugin folder>   and the twin against 0.11's sources: the same samples
#   make sleep OLD12=<0.12's plugin folder> and the build against 0.12's sources: the same samples
# Since 0.13 both are built with the early cut off (-DOP1RINGS_NO_EARLY_SLEEP: the build is then 0.12, which is what
# this target is about; the early cut has its own, `make early`).
#   make sleep-sim                 cycles a block in the simulator, six keys held: the reverb model's default patch,
#                                  and an FM patch whose envelope stalls at 1 LSB
SLEEP_DEPS := rings_sleep.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
SLEEP_SRC = rings_sleep.cc $(1)/rings.cc $(addprefix $(1)/,$(DSP))
$(BUILD)/rings_sleep: $(SLEEP_DEPS)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) $(IDLE_FLAGS) -DOP1RINGS_NO_EARLY_SLEEP -I$(COOP) -I../../../sdk $(call SLEEP_SRC,$(COOP)) -o $@

$(BUILD)/rings_sleep_twin: $(SLEEP_DEPS)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) $(IDLE_FLAGS) -DOP1RINGS_NO_QUIET_SLEEP -I$(COOP) -I../../../sdk $(call SLEEP_SRC,$(COOP)) -o $@

# One comparison, nothing kept on disk: $(1) = the two programs, $(2) = the run's arguments, $(3) = its environment.
SLEEP_CMP = bash -c "$(BUILD)/rings_sleep cmp <($(3) $(word 1,$(1)) run /dev/fd/3 $(2) 3>&1 >/dev/null) \
	  <($(3) $(word 2,$(1)) run /dev/fd/3 $(2) 3>&1 >/dev/null) 6"

sleep: $(BUILD)/rings_sleep $(BUILD)/rings_sleep_twin
	@echo "the default patch, 60 s, voice 0 struck again every 7 s, voice 1 at 40 s: as it ships / its twin"
	@set -e; for m in 0 1 2 3 4 5; do for q in 0 1 2 3; do printf "model $$m, polyphony setting $$q:"; \
	  $(call SLEEP_CMP,$(BUILD)/rings_sleep $(BUILD)/rings_sleep_twin,$$m $$q 22937 60 6 7,env); done; done
ifneq ($(OLD),)
	$(HOST_CXX) $(CXXFLAGS) $(IDLE_FLAGS) -I$(OLD) -I../../../sdk $(call SLEEP_SRC,$(OLD)) -o $(BUILD)/rings_sleep_old
	@echo "the twin / $(OLD):"
	@set -e; for m in 0 1 2 3 4 5; do for q in 0 3; do for d in 22937 28000; do printf "model $$m, polyphony setting $$q, damping $$d:"; \
	  $(call SLEEP_CMP,$(BUILD)/rings_sleep_twin $(BUILD)/rings_sleep_old,$$m $$q $$d 60 6 7,env); done; done; done
endif
ifneq ($(OLD12),)
	$(HOST_CXX) $(CXXFLAGS) $(IDLE_FLAGS) -I$(OLD12) -I../../../sdk $(call SLEEP_SRC,$(OLD12)) -o $(BUILD)/rings_sleep_old12
	@echo "the early cut off / $(OLD12):"
	@set -e; for m in 0 1 2 3 4 5; do for q in 0 1 2 3; do for d in 22937 28000 31000; do printf "model $$m, polyphony setting $$q, damping $$d:"; 	  $(call SLEEP_CMP,$(BUILD)/rings_sleep $(BUILD)/rings_sleep_old12,$$m $$q $$d 60 6 7,env); done; done; done
	@set -e; for c in "0 28000 8192 32767" "3 26000 0 19660" "3 28000 16384 19660" "5 28000 0 19660" "1 28000 0 32767"; do set -- $$c; 	  printf "model $$1, damping $$2, structure $$3, brightness $$4, 150 s:"; 	  $(call SLEEP_CMP,$(BUILD)/rings_sleep $(BUILD)/rings_sleep_old12,$$1 3 $$2 150 6,env SLEEP_STRUCTURE=$$3 SLEEP_BRIGHTNESS=$$4); done
endif
	@echo "tails that stall, 150 s held (a difference is the stalled tail the twin goes on playing):"
	@for c in "0 28000 8192 32767" "3 26000 0 19660" "3 28000 16384 19660" "5 28000 0 19660" "1 28000 0 32767"; do set -- $$c; \
	  printf "model $$1, damping $$2, structure $$3, brightness $$4:"; \
	  $(call SLEEP_CMP,$(BUILD)/rings_sleep $(BUILD)/rings_sleep_twin,$$1 3 $$2 150 6,env SLEEP_STRUCTURE=$$3 SLEEP_BRIGHTNESS=$$4) || true; \
	  SLEEP_QUIET=1 SLEEP_STRUCTURE=$$3 SLEEP_BRIGHTNESS=$$4 $(BUILD)/rings_sleep run - $$1 3 $$2 150 6 | grep voice; \
	  echo " its twin:"; \
	  SLEEP_QUIET=1 SLEEP_STRUCTURE=$$3 SLEEP_BRIGHTNESS=$$4 $(BUILD)/rings_sleep_twin run - $$1 3 $$2 150 6 | grep voice; done

$(BUILD)/rings_sleep.elf: $(SLEEP_DEPS)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) $(IDLE_FLAGS) -DOP1RINGS_NO_EARLY_SLEEP -I$(COOP) -I../../../sdk 	  $(call SLEEP_SRC,$(COOP)) $(ASM) -o $@

$(BUILD)/rings_sleep_twin.elf: $(SLEEP_DEPS)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) $(IDLE_FLAGS) -DOP1RINGS_NO_QUIET_SLEEP -I$(COOP) -I../../../sdk \
	  $(call SLEEP_SRC,$(COOP)) $(ASM) -o $@

sleep-sim: $(BUILD)/rings_sleep.elf $(BUILD)/rings_sleep_twin.elf
	$(BFIN)/bfin-elf-run $(BUILD)/rings_sleep.elf run - 5 3 22937 25 6
	$(BFIN)/bfin-elf-run $(BUILD)/rings_sleep.elf run - 3 3 26000 35 6 0 0
	$(BFIN)/bfin-elf-run $(BUILD)/rings_sleep_twin.elf run - 3 3 26000 35 6 0 0

# rings 0.13: the early cut (src/op1_voice.h kEarlyLevel; rings_early.cc): a voice under -66 dBFS for 5 s is faded
# out over 1 s and sleeps. Four builds of the one test: as it ships; the -60 dBFS build; the twin without the rule
# (-DOP1RINGS_NO_EARLY_SLEEP: 0.12); and the rule with the faded voice's state kept (-DOP1RINGS_EARLY_FREEZE=1).
#   make early                     the proofs: above the level the twin's samples, the fades, the strikes, the steal
#   make early-table               when each voice fades and sleeps, and at what level (0.12, -66, -60), on the host
#   make early-sim                 the simulator against the host (the assembly kernels), and the cost curves
EARLY_FLAGS := $(IDLE_FLAGS) -DCOOP_EARLY_PROBE
EARLY_DEPS := rings_early.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
EARLY_SRC := rings_early.cc $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP))
EARLY_60 := -DOP1RINGS_EARLY_LEVEL=134218
EARLY_VARIANT_ := 
EARLY_VARIANT_60 := $(EARLY_60)
EARLY_VARIANT__twin := -DOP1RINGS_NO_EARLY_SLEEP
EARLY_VARIANT__freeze := -DOP1RINGS_EARLY_FREEZE=1
define EARLY_BUILD
$(BUILD)/rings_early$(1): $(EARLY_DEPS)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) $(EARLY_FLAGS) $(EARLY_VARIANT_$(1)) -I$(COOP) -I../../../sdk $(EARLY_SRC) -o $$@
$(BUILD)/rings_early$(1).elf: $(EARLY_DEPS)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) $(EARLY_FLAGS) $(EARLY_VARIANT_$(1)) -I$(COOP) -I../../../sdk $(EARLY_SRC) $(ASM) -o $$@
endef
$(eval $(call EARLY_BUILD,))
$(eval $(call EARLY_BUILD,60))
$(eval $(call EARLY_BUILD,_twin))
$(eval $(call EARLY_BUILD,_freeze))
EARLY_BINS := $(BUILD)/rings_early $(BUILD)/rings_early60 $(BUILD)/rings_early_twin $(BUILD)/rings_early_freeze
early-bins: $(EARLY_BINS)

early: $(EARLY_BINS)
	bash early.sh $(BUILD) proofs

early-table: $(EARLY_BINS)
	bash early.sh $(BUILD) table

early-sim: $(EARLY_BINS) $(BUILD)/rings_early.elf $(BUILD)/rings_early60.elf $(BUILD)/rings_early_twin.elf
	bash early.sh $(BUILD) sim $(BFIN)

# rings 0.14 against 0.13 (v014.sh): make v014 OLD=<0.13's plugin folder> [V14_BLOCKS=n]. The build as it ships
# (SHIP_FLAGS: plugin.json's cflags), with the string pool poisoned (OP1RINGS_POOL_POISON), with every lease refused,
# and the te-op1-cfw configuration; each against the same build of OLD: every sample the same.
SHIP_FLAGS := -DOP1_COOP -DOP1RINGS_CONTROL_HALVED=1 -DOP1RINGS_CONTROL_BATCHED=1 -DOP1RINGS_COOP_POLY4=1 \
              -DOP1RINGS_COOP_POLY_ALL=1 -DOP1RINGS_VOICE_44K=1
V14_BLOCKS ?= 62010
V14_SRC = rings_compare.cc $(1)/rings.cc $(addprefix $(1)/,$(DSP))
v014: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
ifeq ($(OLD),)
	@echo "make v014 OLD=<0.13's plugin folder>"; exit 2
endif
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) $(SHIP_FLAGS) -I$(COOP) -I../../../sdk $(call V14_SRC,$(COOP)) -o $(BUILD)/rings_v14
	$(HOST_CXX) $(CXXFLAGS) $(SHIP_FLAGS) -DOP1RINGS_POOL_POISON -I$(COOP) -I../../../sdk $(call V14_SRC,$(COOP)) -o $(BUILD)/rings_v14p
	$(HOST_CXX) $(CXXFLAGS) -DOP1_COOP -DOP1RINGS_POOL_POISON -I$(COOP) -I../../../sdk $(call V14_SRC,$(COOP)) -o $(BUILD)/rings_v14n
	$(HOST_CXX) $(CXXFLAGS) $(SHIP_FLAGS) -DOP1RINGS_POOL_STRINGS=15 -I$(COOP) -I../../../sdk $(call V14_SRC,$(COOP)) -o $(BUILD)/rings_v14s
	$(HOST_CXX) $(CXXFLAGS) $(SHIP_FLAGS) -I$(OLD) -I../../../sdk $(call V14_SRC,$(OLD)) -o $(BUILD)/rings_v13
	$(HOST_CXX) $(CXXFLAGS) -DOP1_COOP -I$(OLD) -I../../../sdk $(call V14_SRC,$(OLD)) -o $(BUILD)/rings_v13n
	bash v014.sh $(BUILD) $(OLD) $(V14_BLOCKS)

# rings 0.14 against 0.13 on the long held-key scenarios (v014long.sh: rings_early, rings_sleep, rings_idle as they
# ship, early cut on): make v014-long OLD=<0.13's plugin folder>
v014-long:
ifeq ($(OLD),)
	@echo "make v014-long OLD=<0.13's plugin folder>"; exit 2
endif
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) $(EARLY_FLAGS) -I$(COOP) -I../../../sdk rings_early.cc $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) -o $(BUILD)/early_v14
	$(HOST_CXX) $(CXXFLAGS) $(EARLY_FLAGS) -I$(OLD) -I../../../sdk rings_early.cc $(OLD)/rings.cc $(addprefix $(OLD)/,$(DSP)) -o $(BUILD)/early_v13
	$(HOST_CXX) $(CXXFLAGS) $(IDLE_FLAGS) -I$(COOP) -I../../../sdk $(call SLEEP_SRC,$(COOP)) -o $(BUILD)/sleep_v14
	$(HOST_CXX) $(CXXFLAGS) $(IDLE_FLAGS) -I$(OLD) -I../../../sdk $(call SLEEP_SRC,$(OLD)) -o $(BUILD)/sleep_v13
	$(HOST_CXX) $(CXXFLAGS) $(IDLE_FLAGS) -I$(COOP) -I../../../sdk rings_idle.cc $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) -o $(BUILD)/idle_v14
	$(HOST_CXX) $(CXXFLAGS) $(IDLE_FLAGS) -I$(OLD) -I../../../sdk rings_idle.cc $(OLD)/rings.cc $(addprefix $(OLD)/,$(DSP)) -o $(BUILD)/idle_v13
	bash v014long.sh $(BUILD)

# rings 0.14: the simulator (the assembly kernels) against the host (their C), the build as it ships, rings_compare's
# scenario for each model at polyphony 4 and "all" (V14_SIM_BLOCKS each), and a held six-key chord per model
# (rings_early, 20 s, keys struck again every 4 s and left to die): the same bytes. make v014-sim
V14_SIM_BLOCKS ?= 4000
v014-sim:
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) $(SHIP_FLAGS) -I$(COOP) -I../../../sdk $(call V14_SRC,$(COOP)) -o $(BUILD)/rings_v14
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) $(SHIP_FLAGS) -I$(COOP) -I../../../sdk $(call V14_SRC,$(COOP)) $(ASM) -o $(BUILD)/rings_v14.elf
	$(HOST_CXX) $(CXXFLAGS) $(EARLY_FLAGS) -I$(COOP) -I../../../sdk $(EARLY_SRC) -o $(BUILD)/early_v14
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -DM_PI=3.14159265358979323846 $(EARLY_FLAGS) -I$(COOP) -I../../../sdk $(EARLY_SRC) $(ASM) -o $(BUILD)/early_v14.elf
	bash v014sim.sh $(BUILD) $(BFIN) $(V14_SIM_BLOCKS)

# rings 0.14: the modal coefficient kernels against their C, in the simulator (modal_check.cc).
$(BUILD)/modal_check.elf: modal_check.cc $(COOP)/src/modal_resonator.cc $(COOP)/src/modal_bfin.S $(COOP)/src/fixed_tables.cc
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -DOP1_COOP -I$(COOP) modal_check.cc $(COOP)/src/fixed_tables.cc \
	  $(COOP)/src/mode_bank_bfin.S $(COOP)/src/modal_bfin.S -o $@

modal-check: $(BUILD)/modal_check.elf
	$(BFIN)/bfin-elf-run $<

# rings 0.14: code per render and per block (code_footprint.py) and compute per render, in the simulator, for a plugin
# folder (v014fp.sh): make v014-footprint [FP_SRC=<plugin folder>] [FP_TAG=name]   (~6 min, 36 runs at once)
FP_SRC ?= $(COOP)
FP_TAG ?= new
v014-footprint:
	bash v014fp.sh $(BUILD)/fp $(BFIN) $(FP_SRC) $(FP_TAG)
