# cscript — build, test and debug targets.
#
#   make            optimised build     -> build/release/cscript
#   make debug      -O0 -g + UBSan      -> build/debug/cscript
#   make asan       debug + AddressSan  -> build/asan/cscript
#   make trace      debug + dumps every stage (tokens, AST, bytecode, execution)
#   make test       golden-file suite against the UBSan build
#   make test-asan  golden-file suite against the AddressSanitizer build
#   make test-gc    golden-file suite with a collection on every allocation
#   make run FILE=examples/hello.cs
#   make clean
#
# Each configuration compiles into its own directory. Sharing one would let a
# release object silently satisfy a debug build, producing a binary linked
# against a sanitizer runtime but compiled without instrumentation.
#
# UBSan is in the default debug build because it is portable and cheap. ASan is
# a separate target: it is worth running, but it fails to start under some
# sandboxed environments, and that should not be able to wedge `make test`.

CC      ?= cc
SRC_DIR := src
INC_DIR := include
BUILD   := build

SOURCES := $(wildcard $(SRC_DIR)/*.c)

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
            -Wmissing-prototypes -Wconversion -Wno-sign-conversion \
            -Wpointer-arith -Wcast-qual -Wwrite-strings

BASE_CFLAGS := -std=c11 $(WARNINGS) -I$(INC_DIR) -MMD -MP
LDLIBS      := -lm

# -fno-sanitize-recover makes undefined behaviour abort instead of warn, so a
# UB finding actually fails the test run rather than scrolling past.
UBSAN         := -fsanitize=undefined -fno-sanitize-recover=undefined
ASAN          := -fsanitize=address
TRACE_DEFINES := -DCS_DEBUG_PRINT_TOKENS -DCS_DEBUG_PRINT_AST \
                 -DCS_DEBUG_PRINT_CODE -DCS_DEBUG_TRACE_EXECUTION

.PHONY: all release debug asan gcstress trace test test-asan test-gc run clean help
.DEFAULT_GOAL := release

all: release

# $(1) configuration name, $(2) extra compile flags, $(3) extra link flags
define BUILD_CONFIG
$(1)_DIR     := $(BUILD)/$(1)
$(1)_OBJECTS := $$(patsubst $(SRC_DIR)/%.c,$(BUILD)/$(1)/%.o,$(SOURCES))
$(1)_BIN     := $(BUILD)/$(1)/cscript

$(BUILD)/$(1)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $$(@D)
	$(CC) $(BASE_CFLAGS) $(2) -c $$< -o $$@

$$($(1)_BIN): $$($(1)_OBJECTS)
	$(CC) $(3) -o $$@ $$^ $(LDLIBS)

$(1): $$($(1)_BIN)

-include $$($(1)_OBJECTS:.o=.d)
endef

$(eval $(call BUILD_CONFIG,release,-O2 -DNDEBUG,))
$(eval $(call BUILD_CONFIG,debug,-O0 -g3 $(UBSAN) -fno-omit-frame-pointer,$(UBSAN)))
$(eval $(call BUILD_CONFIG,asan,-O0 -g3 $(ASAN) $(UBSAN) -fno-omit-frame-pointer,$(ASAN) $(UBSAN)))
$(eval $(call BUILD_CONFIG,gcstress,-O0 -g3 $(UBSAN) -DCS_DEBUG_STRESS_GC,$(UBSAN)))
$(eval $(call BUILD_CONFIG,trace,-O0 -g3 $(UBSAN) $(TRACE_DEFINES),$(UBSAN)))

# Run against an instrumented build so undefined behaviour fails the suite.
test: debug
	@BIN=$(BUILD)/debug/cscript tests/run_tests.sh $(FILTER)

test-asan: asan
	@BIN=$(BUILD)/asan/cscript tests/run_tests.sh $(FILTER)

# Collects on every allocation, so any value the collector cannot reach from a
# root is freed the instant it becomes unreachable. This is the cheapest way to
# find a missing GC root, and it has already caught one.
test-gc: gcstress
	@BIN=$(BUILD)/gcstress/cscript tests/run_tests.sh $(FILTER)

run: release
	@$(BUILD)/release/cscript $(FILE)

clean:
	rm -rf $(BUILD)

help:
	@echo "make          optimised build -> build/release/cscript"
	@echo "make debug    UBSan build     -> build/debug/cscript"
	@echo "make asan     AddressSanitizer build"
	@echo "make trace    debug build + stage dumps"
	@echo "make test     golden-file suite (FILTER=name to narrow)"
	@echo "make test-asan  same suite under AddressSanitizer"
	@echo "make test-gc    same suite, collecting on every allocation"
	@echo "make run FILE=examples/hello.cs"
	@echo "make clean"
