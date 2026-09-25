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

.PHONY: all clean check test test-unit test-target test-bochs self-host selfhost-probe fuzz-smoke repro-check check-release check-release-strict audit install-tools
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

build/libcc64core.a: $(filter-out build/src/driver/main.o,$(OBJECTS))
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

test-unit: build/cc64-unit build/cc64-frontend build/cc64-semantic build/cc64-numeric
	@build/cc64-unit
	@build/cc64-frontend
	@build/cc64-semantic
	@build/cc64-numeric

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

fuzz-smoke: cc64
	@python3 tests/fuzz_smoke.py

repro-check: cc64
	@python3 tools/repro_check.py

check-release: check test-target test-bochs self-host selfhost-probe fuzz-smoke repro-check audit
check-release-strict:
	@CC64_REQUIRE_CLEAN=1 python3 tests/audit.py
	@CC64_REQUIRE_CLEAN=1 CC64_REQUIRE_EMULATORS=1 $(MAKE) check-release
.NOTPARALLEL: check-release check-release-strict

audit:
	@python3 tests/audit.py

check: test audit

clean:
	rm -rf build cc64

-include $(DEPS)
