# KernQueue top-level Makefile.
#
#   make                 build the user-space programs into build/
#   make module          build driver/kernqueue.ko (needs KDIR, see README)
#   make test-unit       ring-logic unit test (no driver needed)
#   make test-harness    stress harness self-test against a FIFO (no driver)
#   make test            functional tests against the loaded driver
#   make stress          concurrency/integrity suite against the loaded driver
#   make bench           benchmark matrix against the loaded driver
#   make sanitize        unit + harness tests under ASan/UBSan
#   make clean
#
# Device targets fail (nonzero) if /dev/kernqueue is missing; they never skip.
# Paths here are relative on purpose: the repository may live in a directory
# whose name contains spaces.

CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes \
           -Wmissing-prototypes -Wpointer-arith -Wcast-align -Wwrite-strings \
           -Wundef -Wvla -Werror
CPPFLAGS += -D_GNU_SOURCE -Iinclude -Iuser
LDLIBS  += -pthread

ifdef SANITIZE
BUILD   ?= build/san
CFLAGS  += -fsanitize=$(SANITIZE) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SANITIZE)
else
BUILD   ?= build
endif

ALL_CFLAGS := -std=c11 $(WARN) $(CFLAGS)

PROGS := $(BUILD)/kqctl $(BUILD)/test_ring $(BUILD)/test_functional \
         $(BUILD)/test_stress $(BUILD)/kq_bench

.PHONY: all user module test-unit test-harness test stress bench sanitize \
        check-device clean

all: user
user: $(PROGS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: user/%.c include/kernqueue.h user/kq_common.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -c $< -o $@

$(BUILD)/%.o: tests/%.c include/kernqueue.h user/kq_common.h tests/kq_test.h | $(BUILD)
	$(CC) $(CPPFLAGS) -Idriver $(ALL_CFLAGS) -c $< -o $@

$(BUILD)/%.o: bench/%.c include/kernqueue.h user/kq_common.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -c $< -o $@

$(BUILD)/test_ring.o: driver/kq_ring.h

$(BUILD)/kqctl: $(BUILD)/kqctl.o $(BUILD)/kq_common.o
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_ring: $(BUILD)/test_ring.o
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_functional: $(BUILD)/test_functional.o $(BUILD)/kq_test.o $(BUILD)/kq_common.o
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_stress: $(BUILD)/test_stress.o $(BUILD)/kq_common.o
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/kq_bench: $(BUILD)/kq_bench.o $(BUILD)/kq_common.o
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

module:
	scripts/build_module.sh

test-unit: $(BUILD)/test_ring
	$(BUILD)/test_ring

test-harness: $(BUILD)/test_stress
	scripts/test_harness_fifo.sh $(BUILD)/test_stress

test: $(BUILD)/test_functional
	$(BUILD)/test_functional

stress: $(BUILD)/test_stress
	scripts/run_stress.sh $(BUILD)/test_stress

bench: $(BUILD)/kq_bench
	bench/run_bench.sh $(BUILD)/kq_bench

sanitize:
	$(MAKE) SANITIZE=address,undefined user
	$(MAKE) SANITIZE=address,undefined test-unit test-harness

# Load the module, run functional + stress tests, check dmesg, unload.
check-device: user module
	scripts/run_device_tests.sh

clean:
	rm -rf build
	rm -f driver/*.o driver/*.ko driver/*.mod driver/*.mod.c driver/.*.cmd \
	      driver/Module.symvers driver/modules.order
