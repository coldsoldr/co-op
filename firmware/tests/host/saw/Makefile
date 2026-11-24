# The saw engine's cycles and digest in the Blackfin GNU simulator (saw_bench.cc), saw.cc compiled as the packer
# compiles it (tools/plugin/pack.py CFLAGS + plugin.json "source_cflags"), and once more without its source_cflags
# for the A/B. Both must print the digest below: the output is bit-identical.
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/saw sim'
COOP := ../../../plugins/saw
BUILD ?= ../../../build/host/saw
BFIN ?= $(HOME)/op1-toolchain/bfin/bin
DIGEST := 967d6f58
PACKFLAGS := -O2 -mcpu=bf524 -ffreestanding -fno-builtin -nostdlib -fno-common -Wall -ffunction-sections -fdata-sections \
             -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -I../../../sdk -I../../../core/include
SRCFLAGS := $(shell python3 -c "import json; print(' '.join(json.load(open('$(COOP)/plugin.json')).get('source_cflags', {}).get('saw.cc', [])))")
SIMFLAGS := -O2 -mcpu=bf524 -msim -std=c++17 -fno-exceptions -fno-rtti -I../../../sdk

.PHONY: sim clean
sim: $(BUILD)/saw_bench.elf $(BUILD)/saw_bench_base.elf
	@echo "== saw.cc as packed ($(SRCFLAGS))"
	$(BFIN)/bfin-elf-run $(BUILD)/saw_bench.elf | tee $(BUILD)/packed.txt
	@echo "== saw.cc without its source_cflags"
	$(BFIN)/bfin-elf-run $(BUILD)/saw_bench_base.elf | tee $(BUILD)/base.txt
	grep -q "digest $(DIGEST)" $(BUILD)/packed.txt && grep -q "digest $(DIGEST)" $(BUILD)/base.txt && \
	  echo "saw: both builds give digest $(DIGEST), bit-identical"

$(BUILD)/bench.o: saw_bench.cc ../../../sdk/coop_plugin.h
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -c $< -o $@

$(BUILD)/saw.o: $(COOP)/saw.cc $(COOP)/plugin.json
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(PACKFLAGS) $(SRCFLAGS) -c $< -o $@

$(BUILD)/saw_base.o: $(COOP)/saw.cc
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(PACKFLAGS) -c $< -o $@

$(BUILD)/saw_bench.elf: $(BUILD)/bench.o $(BUILD)/saw.o
	$(BFIN)/bfin-elf-g++ -mcpu=bf524 -msim $^ -o $@

$(BUILD)/saw_bench_base.elf: $(BUILD)/bench.o $(BUILD)/saw_base.o
	$(BFIN)/bfin-elf-g++ -mcpu=bf524 -msim $^ -o $@

clean:
	rm -rf $(BUILD)
