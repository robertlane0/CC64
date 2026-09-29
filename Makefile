CC ?= cc
AR ?= ar
CFLAGS ?= -O2
WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Werror
CPPFLAGS += -Isrc -Iinclude
ALL_CFLAGS = -std=c17 $(WARNINGS) $(CFLAGS)

SOURCES = $(shell find src -name '*.c' ! -path 'src/selfhost/*' ! -path 'src/runtime/*' -print | sort)
OBJECTS = $(SOURCES:%.c=build/%.o)
DEPS = $(OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)
TEST_SOURCES = $(shell find tests/unit -name '*.c' -print | sort 2>/dev/null)
TEST_OBJECTS = $(TEST_SOURCES:%.c=build/%.o)

.PHONY: all clean check test test-unit test-target test-bochs self-host self-host-stage selfhost-probe self-host-run self-host-corpus fuzz-smoke repro-check check-release check-release-strict audit target-lib link-negative compat-matrix perf-measure
all: cc64

cc64: $(OBJECTS)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

build/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

build/cc64-unit: build/tests/unit/test_common.o build/libcc64core.a
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ build/tests/unit/test_common.o build/libcc64core.a $(LDLIBS)

build/cc64-frontend: build/tests/unit/test_frontend.o build/libcc64core.a
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ build/tests/unit/test_frontend.o build/libcc64core.a $(LDLIBS)

build/cc64-semantic: build/tests/unit/test_semantic.o build/libcc64core.a
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ build/tests/unit/test_semantic.o build/libcc64core.a $(LDLIBS)

build/cc64-numeric: build/tests/unit/test_numeric.o build/libcc64core.a
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ build/tests/unit/test_numeric.o build/libcc64core.a $(LDLIBS)

build/cc64-abi: build/tests/unit/test_abi.o build/libcc64core.a
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ build/tests/unit/test_abi.o build/libcc64core.a $(LDLIBS)

build/cc64-diagnostic: build/tests/unit/test_diagnostic.o build/libcc64core.a
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ build/tests/unit/test_diagnostic.o build/libcc64core.a $(LDLIBS)

build/cc64-conversion: build/tests/unit/test_conversion.o build/libcc64core.a
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ build/tests/unit/test_conversion.o build/libcc64core.a $(LDLIBS)

build/libcc64core.a: $(filter-out build/src/driver/main.o,$(OBJECTS))
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

test-unit: build/cc64-unit build/cc64-frontend build/cc64-semantic build/cc64-numeric build/cc64-abi build/cc64-diagnostic build/cc64-conversion
	@build/cc64-unit
	@build/cc64-frontend
	@build/cc64-semantic
	@build/cc64-numeric
	@build/cc64-abi
	@build/cc64-diagnostic
	@build/cc64-conversion

test: test-unit
	@python3 tests/run.py

test-target: cc64
	@python3 tests/run_target.py

test-bochs: cc64
	@python3 tests/run_bochs.py

self-host: cc64
	@python3 tools/self_host.py

selfhost-probe: cc64
	@python3 tools/selfhost_probe.py

self-host-run: cc64
	@python3 tools/selfhost_run.py

# Compile the compiler on the target with the target-built compiler and compare
# every object and the relinked image with the bootstrap build.
self-host-stage: cc64
	@python3 tools/selfhost_stage.py

# Compile, link, and run the whole target conformance corpus with a compiler
# the target built, and compare every object with the bootstrap compiler's.
self-host-corpus: cc64
	@python3 tools/selfhost_corpus.py

target-lib: cc64
	@python3 tools/target_lib.py

# Every rejection the linker documents, driven by one field of a valid object.
link-negative: cc64
	@python3 tests/link_negative.py

# Both image forms against the pinned contract, and a version this compiler
# does not write required to be refused.
compat-matrix: cc64
	@python3 tests/compat_matrix.py

# Record what the compiler and the target cost, against the bounds the
# contracts state. A measurement outside a bound fails rather than reporting.
perf-measure: cc64
	@python3 tools/perf_measure.py

fuzz-smoke: cc64
	@python3 tests/fuzz_smoke.py

repro-check: cc64
	@python3 tools/repro_check.py

# The release gate treats a missing emulator as a failure rather than a skip,
# so it cannot record that a target property was checked when nothing booted.
check-release: export CC64_REQUIRE_EMULATORS = 1
check-release: check test-target test-bochs self-host self-host-stage selfhost-probe self-host-run self-host-corpus target-lib link-negative compat-matrix perf-measure fuzz-smoke repro-check audit
check-release-strict: export CC64_REQUIRE_CLEAN = 1
check-release-strict: export CC64_REQUIRE_EMULATORS = 1
check-release-strict: audit
	@$(MAKE) check-release
.NOTPARALLEL: check-release check-release-strict

audit:
	@python3 tests/audit.py

check: test audit

clean:
	rm -rf build cc64

-include $(DEPS)
