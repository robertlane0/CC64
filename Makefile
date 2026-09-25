CC ?= cc
AR ?= ar
CFLAGS ?= -O2
WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Werror
CPPFLAGS += -Isrc -Iinclude
ALL_CFLAGS = -std=c17 $(WARNINGS) $(CFLAGS)

SOURCES = $(shell find src -name '*.c' -print | sort)
OBJECTS = $(SOURCES:%.c=build/%.o)
DEPS = $(OBJECTS:.o=.d)
TEST_SOURCES = $(shell find tests/unit -name '*.c' -print | sort 2>/dev/null)
TEST_OBJECTS = $(TEST_SOURCES:%.c=build/%.o)

.PHONY: all clean check test test-unit audit install-tools
all: cc64

cc64: $(OBJECTS)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

build/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

build/cc64-unit: $(TEST_OBJECTS) build/libcc64core.a
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(TEST_OBJECTS) build/libcc64core.a $(LDLIBS)

build/libcc64core.a: $(filter-out build/src/driver/main.o,$(OBJECTS))
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

test-unit: build/cc64-unit
	@build/cc64-unit

test: test-unit
	@python3 tests/run.py

audit:
	@python3 tests/audit.py

check: test audit

clean:
	rm -rf build cc64

-include $(DEPS)
