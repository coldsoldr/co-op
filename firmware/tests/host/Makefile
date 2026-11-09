# Host unit tests (native). Each tests/host/*_test.cc builds to one executable and runs.
HOST_CXX ?= g++
HOST_CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -DOP1_HOST
BUILD ?= ../../build/host
SRCS := $(wildcard *_test.cc)
BINS := $(patsubst %_test.cc,$(BUILD)/%_test,$(SRCS))

.PHONY: all clean
all: $(BINS)
	@for b in $(BINS); do echo "== $$b"; $$b || exit 1; done
	@test -n "$(BINS)" || echo "no host tests yet"

$(BUILD)/%_test: %_test.cc
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) -I../../sdk -I../../core/include $< -o $@

clean:
	rm -rf $(BUILD)
