# Super_RCT build.  Usage: make            (build build/super_rct)
#                          make test       (run integration tests)
#                          make size       (show binary size)
#                          make gen        (regenerate command tables)
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g
CFLAGS  += -Wall -Wextra -Wpedantic -Wshadow -Wformat=2
LDFLAGS ?=

BUILD   := build
TARGET  := $(BUILD)/super_rct
SRC     := src/super_rct.c

RELFLAGS := -Os -s -ffunction-sections -fdata-sections
RELLDFLAGS := -Wl,--gc-sections

.PHONY: all release test size gen clean

all: $(TARGET)

$(TARGET): $(SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC)

# Smallest binary: stripped, size-optimised, dead code removed.
release: $(SRC) | $(BUILD)
	$(CC) -std=c11 $(RELFLAGS) -Wall -Wextra -Wpedantic $(LDFLAGS) $(RELLDFLAGS) -o $(TARGET) $(SRC)

$(BUILD):
	mkdir -p $(BUILD)

test: all
	python3 tests/test_super_rct.py

gen:
	python3 tools/gen_commands.py

size: release
	@ls -l $(TARGET) | awk '{printf "%s: %d bytes\n", $$9, $$5}'

clean:
	rm -rf $(BUILD)
