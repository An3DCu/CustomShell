# Builds acsh and my own tools, and runs the tests.
#
#   make              build/acsh and build/tools/*
#   make debug        the same with AddressSanitizer and UBSan, in build/debug/
#   make test         unit tests, then end-to-end tests
#   make test-debug   the same tests on the sanitizer build
#   make clean        remove build/ (the test venv in .venv/ stays)
#
# Works with CC=gcc and CC=clang, e.g. `make CC=clang test`.

MODE ?= release

ifeq ($(MODE),debug)
  BUILD    := build/debug
  OPT      := -O1 -g -fno-omit-frame-pointer
  SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=all
else ifeq ($(MODE),release)
  BUILD    := build
  OPT      := -O2
  SANITIZE :=
else
  $(error MODE must be release or debug)
endif

# C17 plus the POSIX.1-2008 functions (getline, strdup, sigaction, ...).
# -MMD -MP write a .d file next to each object, so editing a header rebuilds
# whatever includes it.
CPPFLAGS := -D_POSIX_C_SOURCE=200809L -Isrc -MMD -MP

# Every warning is an error. The extra ones:
#   -Wshadow               a local name hides an outer one
#   -Wstrict-prototypes    f() instead of f(void)
#   -Wmissing-prototypes   a non-static function no header declares
#   -Wformat=2             printf-style calls with formats that can't be checked
#   -Wundef                #if on a macro that was never defined
#   -Wvla                  variable-length arrays (stack size from user input)
WARNINGS := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wstrict-prototypes \
            -Wmissing-prototypes -Wformat=2 -Wundef -Wvla

CFLAGS  := -std=c17 $(WARNINGS) $(OPT) $(SANITIZE)
LDFLAGS := $(SANITIZE)

SRC      := $(wildcard src/*.c)
OBJ      := $(SRC:src/%.c=$(BUILD)/obj/%.o)
LIB_OBJ  := $(filter-out $(BUILD)/obj/main.o,$(OBJ))

TOOL_SRC := $(wildcard tools/*.c)
TOOLS    := $(TOOL_SRC:tools/%.c=$(BUILD)/tools/%)
TOOL_LIB := $(BUILD)/toollib/tool.o

UNIT_SRC := $(wildcard tests/unit/test_*.c)
UNIT     := $(UNIT_SRC:tests/unit/%.c=$(BUILD)/unit/%)
CHECK    := $(BUILD)/unit/check.o

# The end-to-end tests run in their own Python venv. `python -m` instead of the
# venv's scripts, because a script's #! line breaks on a path with spaces.
VENV := .venv
PY   := $(VENV)/bin/python

.PHONY: all debug test test-unit test-e2e test-debug test-valgrind fuzz fuzz-merge clean

# Keep the objects make builds on the way to a test binary. Without this it
# treats them as intermediate files, deletes them, and recompiles every run.
.SECONDARY:

all: $(BUILD)/acsh $(TOOLS)

$(BUILD)/acsh: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# Each tool is one source file and its own program, linked with the few
# helpers they share (tools/lib/).
$(BUILD)/toollib/%.o: tools/lib/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/tools/%: tools/%.c $(TOOL_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itools/lib $(CFLAGS) $(LDFLAGS) -o $@ $< $(TOOL_LIB)

# A unit test links the harness and every shell module except main.
$(BUILD)/unit/%.o: tests/unit/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(CFLAGS) -c -o $@ $<

$(BUILD)/unit/test_%: $(BUILD)/unit/test_%.o $(CHECK) $(LIB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(VENV)/ready: tests/requirements.txt
	python3 -m venv $(VENV)
	$(PY) -m pip install --quiet --disable-pip-version-check -r tests/requirements.txt
	@touch $@

test: test-unit test-e2e

test-unit: $(UNIT)
	@for t in $(UNIT); do $$t || exit 1; done

test-e2e: all $(VENV)/ready
	ACSH=$(BUILD)/acsh TOOLS=$(BUILD)/tools $(PY) -m pytest -q tests/e2e

debug:
	$(MAKE) MODE=debug all

test-debug:
	$(MAKE) MODE=debug test

# The end-to-end tests again, with acsh and every tool under valgrind. Slow:
# a few minutes. CI runs it on every PR.
test-valgrind: all $(VENV)/ready
	BUILD=$(BUILD) sh tests/valgrind.sh

# Fuzzing the lexer and parser with libFuzzer, which needs clang. The target
# also has AddressSanitizer and UBSan, so any crash, bad read, undefined
# behavior or leak stops it, and the input that did it lands in build/fuzz/.
# The corpus in tests/fuzz/corpus is read only; what a run finds goes to
# build/fuzz/new. `make fuzz-merge` adds the inputs from there that reach a
# branch the corpus doesn't (edges only, not how often each runs), which
# keeps it small: the seeds and one input per new branch.
FUZZ_DIR  := build/fuzz
FUZZ_BIN  := $(FUZZ_DIR)/fuzz_parser
FUZZ_SRC  := tests/fuzz/fuzz_parser.c src/lexer.c src/parser.c src/vars.c src/strvec.c src/xalloc.c
FUZZ_TIME ?= 120

$(FUZZ_BIN): $(FUZZ_SRC)
	@mkdir -p $(@D)
	clang -D_POSIX_C_SOURCE=200809L -Isrc -std=c17 $(WARNINGS) -O1 -g \
	    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -o $@ $(FUZZ_SRC)

fuzz: $(FUZZ_BIN)
	@mkdir -p $(FUZZ_DIR)/new
	$(FUZZ_BIN) -max_total_time=$(FUZZ_TIME) -dict=tests/fuzz/shell.dict -max_len=4096 \
	    -artifact_prefix=$(FUZZ_DIR)/ $(FUZZ_DIR)/new tests/fuzz/corpus

fuzz-merge: $(FUZZ_BIN)
	$(FUZZ_BIN) -merge=1 -use_counters=0 tests/fuzz/corpus $(FUZZ_DIR)/new

clean:
	rm -rf build

-include $(OBJ:.o=.d) $(UNIT:%=%.d) $(CHECK:.o=.d) $(TOOLS:%=%.d) $(TOOL_LIB:.o=.d)
