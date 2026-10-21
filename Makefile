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

# Which chat protocol the binary serves: both (default), telegram or discord.
PROTOCOL ?= both
ifeq ($(PROTOCOL),both)
PROTO_ID := 0
else ifeq ($(PROTOCOL),telegram)
PROTO_ID := 1
else ifeq ($(PROTOCOL),discord)
PROTO_ID := 2
else
$(error PROTOCOL must be both, telegram or discord)
endif
CFLAGS  += -DSRCT_PROTOCOL=$(PROTO_ID)

BUILD   := build
TARGET  := $(BUILD)/super_rct
SRC     := src/super_rct.c
DEPS    := $(wildcard src/*.h)

RELFLAGS := -Os -s -ffunction-sections -fdata-sections
RELLDFLAGS := -Wl,--gc-sections
# macOS's linker has no --gc-sections; it uses -dead_strip instead.
ifeq ($(shell uname -s),Darwin)
RELFLAGS := -Os -ffunction-sections -fdata-sections
RELLDFLAGS := -Wl,-dead_strip
endif

.PHONY: all release test test-bot-c test-bot-py test-bot-table test-installers test-builders test-update size gen check-gen update clean

all: $(TARGET)

# The stamp records which protocol the binary was built for. Changing PROTOCOL
# rewrites it, which forces `make all` to rebuild the binary for that protocol.
$(BUILD)/protocol.txt: FORCE | $(BUILD)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(PROTOCOL)" ]; then echo $(PROTOCOL) > $@; fi

FORCE:

$(TARGET): $(SRC) $(DEPS) $(BUILD)/protocol.txt | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC)

# Smallest binary: stripped, size-optimised, dead code removed.
release: $(SRC) $(DEPS) $(BUILD)/protocol.txt | $(BUILD)
	$(CC) -std=c11 $(RELFLAGS) -Wall -Wextra -Wpedantic -DSRCT_PROTOCOL=$(PROTO_ID) $(LDFLAGS) $(RELLDFLAGS) -o $(TARGET) $(SRC)

$(BUILD):
	mkdir -p $(BUILD)

# The bot suite is the slow part. Its two variants and the rest run in parallel
# (make -j); -O keeps each target's output together. `all` is built once,
# first, by `test`, so the parallel targets never rebuild the binary at the same time.
TEST_JOBS ?= 4
# The installer and builder suites run `make release` for other protocols, which
# overwrites build/super_rct. The C bot tests therefore use a private copy.
test: all
	mkdir -p $(BUILD)/test && cp $(TARGET) $(BUILD)/test/super_rct
	$(MAKE) -j$(TEST_JOBS) -O test-bot-c test-bot-py test-bot-table test-installers test-builders test-update

test-bot-c:
	SUPER_RCT_BINARY=$(CURDIR)/$(BUILD)/test/super_rct python3 tests/test_super_rct.py CBotTests

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
