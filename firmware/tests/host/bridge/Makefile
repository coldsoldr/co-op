# coop's bridge on the host (core/bridge, docs/bridge.md B1): the loader's path for a .knl (header made coop's dialect,
# the one parser, the import table, relocation against the bridge's exports), the .opmod checks, the dual-mono effect
# adapter, the address audit. Fixtures are made in the test; then, when this checkout has local (gitignored) .knl /
# .opmod files, the core's verdict on each is compared with tools/bridge/survey.py's (SKIP when there are none).
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/bridge'
#   ... make -C tests/host/bridge FILES="dir-or-files"     (the real-file comparison on other files)
HOST_CXX ?= g++
BUILD ?= ../../../build/host/bridge
BR := ../../../core/bridge
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -Wno-unused-function -DOP1_HOST
SRCS := bridge_test.cc $(BR)/bridge_knl.cc $(BR)/bridge_opmod.cc $(BR)/bridge_fx.cc $(BR)/bridge_shims.cc \
        ../../../core/sdram/plugin_load.cc
FILES ?=

.PHONY: all clean
all: $(BUILD)/bridge_test
	$(BUILD)/bridge_test
	python3 ../../../tools/bridge/survey.py --check $(BUILD)/bridge_test $(FILES)

$(BUILD)/bridge_test: $(SRCS) $(BR)/bridge.h $(BR)/bridge_imports.def
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) $(SRCS) -o $@

clean:
	rm -rf $(BUILD)
