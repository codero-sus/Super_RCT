# Super_RCT build.  Usage: make            (build build/super_rct)
#                          make test       (run integration tests)
#                          make size       (show binary size)
#                          make gen        (regenerate command tables)
#                          make check-gen  (fail if command tables are stale)
#                          make update     (show what a git pull would change)
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g
CFLAGS  += -Wall -Wextra -Wpedantic -Wshadow -Wformat=2
LDFLAGS ?=

BUILD   := build
TARGET  := $(BUILD)/super_rct
SRC     := src/super_rct.c
DEPS    := $(wildcard src/*.h)

RELFLAGS := -Os -s -ffunction-sections -fdata-sections
RELLDFLAGS := -Wl,--gc-sections

.PHONY: all release test test-bot-c test-bot-py test-bot-table test-installers test-builders test-update size gen check-gen update clean

all: $(TARGET)

$(TARGET): $(SRC) $(DEPS) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC)

# Smallest binary: stripped, size-optimised, dead code removed.
release: $(SRC) $(DEPS) | $(BUILD)
	$(CC) -std=c11 $(RELFLAGS) -Wall -Wextra -Wpedantic $(LDFLAGS) $(RELLDFLAGS) -o $(TARGET) $(SRC)

$(BUILD):
	mkdir -p $(BUILD)

# The bot suite is the slow part. Its two variants and the rest run in parallel
# (make -j); -O keeps each target's output together. `all` is built once,
# first, by `test`, so the parallel targets never rebuild the binary at the same time.
TEST_JOBS ?= 4
test: all
	$(MAKE) -j$(TEST_JOBS) -O test-bot-c test-bot-py test-bot-table test-installers test-builders test-update

test-bot-c:
	python3 tests/test_super_rct.py CBotTests

test-bot-py:
	python3 tests/test_super_rct.py PythonBotTests

test-bot-table:
	python3 tests/test_super_rct.py GeneratedTableTests

test-installers:
	python3 tests/test_installers.py

test-builders:
	python3 tests/test_builder.py
	python3 tests/test_gui_builder.py

test-update:
	python3 tests/test_update.py

gen:
	python3 tools/gen_commands.py

update:
	python3 tools/update.py

check-gen:
	python3 tools/gen_commands.py --check

size: release
	@ls -l $(TARGET) | awk '{printf "%s: %d bytes\n", $$9, $$5}'

clean:
	rm -rf $(BUILD)
