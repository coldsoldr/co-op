# coop plugins: every folder with a plugin.json packs to build/plugins/<id>.coop (tools/plugin/pack.py).
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C plugins'
TOOLCHAIN ?= $(HOME)/op1-toolchain
OUT := ../build/plugins
IDS := $(patsubst %/plugin.json,%,$(wildcard */plugin.json))
FILES := $(foreach id,$(IDS),$(OUT)/$(id).coop)

all: $(FILES)

$(OUT)/%.coop: %/plugin.json %/*.cc ../sdk/coop_plugin.h ../tools/plugin/pack.py ../tools/plugin/reloc.py | $(OUT)
	python3 -I ../tools/plugin/pack.py $* -o $@ --toolchain $(TOOLCHAIN)

$(OUT):
	mkdir -p $(OUT)

clean:
	rm -f $(FILES)

.PHONY: all clean
