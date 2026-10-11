# W5500 async driver — host build (core library + unit tests + fuzzing)
#
#   make test   build and run the unit tests against the W5500 emulator
#   make lib    build build/libw5500.a (core only, for host-side use)
#   make fuzz-all         fuzz every harness (Clang + libFuzzer)
#   make clean
#
# Cross builds for STM32 are done by your firmware project; the port
# sources under ports/ are compiled with the CMSIS device headers.

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -std=c99 -Werror -g
CPPFLAGS = -Iinclude -Isrc

BUILD    := build
CORE_SRC := src/w5500.c src/w5500_sockets.c
CORE_OBJ := $(BUILD)/w5500.o $(BUILD)/w5500_sockets.o
TEST_SRC := tests/test_w5500.c tests/mock_w5500.c

.PHONY: all test lib clean

all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/w5500.o: src/w5500.c src/w5500_internal.h src/w5500_registers.h \
                   include/w5500.h include/w5500_sockets.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/w5500_sockets.o: src/w5500_sockets.c src/w5500_internal.h \
                          src/w5500_registers.h include/w5500.h \
                          include/w5500_sockets.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/libw5500.a: $(CORE_OBJ)
	ar rcs $@ $(CORE_OBJ)

lib: $(BUILD)/libw5500.a

$(BUILD)/test_w5500.exe: $(CORE_SRC) $(TEST_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Itests $(CORE_SRC) $(TEST_SRC) -o $@

test: $(BUILD)/test_w5500.exe
	./$(BUILD)/test_w5500.exe

clean:
	rm -rf $(BUILD)

# =============================================================================
# FUZZ TARGETS (requires Clang; libFuzzer by default, ASan/UBSan fallback)
#
#   make fuzz-all                       all harnesses (libFuzzer, 120 s each)
#   make fuzz-reg-access                a single harness
#   FUZZ_MODE=asan make fuzz-all        standalone runner + ASan/UBSan
#   FUZZ_TIME=60 make fuzz-all          seconds per target
#   FUZZ_MODE=none make fuzz-reg-access plain build, run checked-in corpus
# =============================================================================

FUZZ_CC      ?= clang
FUZZ_MODE    ?= fuzzer
FUZZ_TIME    ?= 120
FUZZ_OUT     := build/fuzz
FUZZ_SEEDS   := tests/fuzz/corpus
FUZZ_DICT    := tests/fuzz/w5500.dict
FUZZ_CORPUS  := $(FUZZ_OUT)/corpus
FUZZ_SRC     := $(CORE_SRC) tests/mock_w5500.c tests/fuzz/fuzz_runner.c

ifeq ($(FUZZ_MODE),fuzzer)
FUZZ_SAN     := -fsanitize=fuzzer,address,undefined
FUZZ_DEFINE  := -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
else ifeq ($(FUZZ_MODE),asan)
FUZZ_SAN     := -fsanitize=address,undefined
FUZZ_DEFINE  := -DFUZZ_STANDALONE_RUNNER
else ifeq ($(FUZZ_MODE),none)
FUZZ_SAN     :=
FUZZ_DEFINE  := -DFUZZ_STANDALONE_RUNNER
else
$(error FUZZ_MODE must be one of: fuzzer, asan, none)
endif

FUZZ_CFLAGS  = $(FUZZ_SAN) -g -O1 -std=c11 -Wall -Wextra \
               -DW5500_TEST_HARNESS -D_CRT_SECURE_NO_WARNINGS $(FUZZ_DEFINE) \
               -Iinclude -Isrc -Itests
FUZZ_LDFLAGS = $(FUZZ_SAN)

# Native Windows builds need clang_rt.asan_dynamic-x86_64.dll next to the
# executable; cp is a silent no-op on hosts that do not ship it (Linux CI).
FUZZ_RES     = $(shell $(FUZZ_CC) -print-resource-dir 2>/dev/null)
FUZZ_ASAN_DLL = "$(FUZZ_RES)/lib/windows/clang_rt.asan_dynamic-x86_64.dll"

# Every harness shares the checked-in seed blobs and the command dictionary.
# Fuzzing writes into a per-target corpus under build/ so a run never dirties
# the checkout; CI caches $(FUZZ_CORPUS) to carry coverage across runs.
define run_fuzz
	-@mkdir -p $(FUZZ_CORPUS)/$(1)
	-@cp -n $(FUZZ_SEEDS)/* $(FUZZ_CORPUS)/$(1)/ 2>/dev/null || true
	-@cp $(FUZZ_ASAN_DLL) $(FUZZ_OUT)/ 2>/dev/null || true
	$(FUZZ_OUT)/$(1).exe $(FUZZ_CORPUS)/$(1) $(FUZZ_SEEDS) -dict=$(FUZZ_DICT) -max_total_time=$(FUZZ_TIME) -print_final_stats=1
endef

.PHONY: fuzz-reg-access fuzz-socket-lifecycle fuzz-poll fuzz-send-recv \
        fuzz-net-config fuzz-context fuzz-spi-frame fuzz-all

$(FUZZ_OUT):
	mkdir -p $@

fuzz-reg-access: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_reg_access.c $(FUZZ_SRC) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_reg_access.exe
	$(call run_fuzz,fuzz_reg_access)

fuzz-socket-lifecycle: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_socket_lifecycle.c $(FUZZ_SRC) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_socket_lifecycle.exe
	$(call run_fuzz,fuzz_socket_lifecycle)

fuzz-poll: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_poll.c $(FUZZ_SRC) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_poll.exe
	$(call run_fuzz,fuzz_poll)

fuzz-send-recv: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_send_recv.c $(FUZZ_SRC) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_send_recv.exe
	$(call run_fuzz,fuzz_send_recv)

fuzz-net-config: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_net_config.c $(FUZZ_SRC) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_net_config.exe
	$(call run_fuzz,fuzz_net_config)

fuzz-context: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_context.c $(FUZZ_SRC) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_context.exe
	$(call run_fuzz,fuzz_context)

fuzz-spi-frame: | $(FUZZ_OUT)
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_spi_frame.c $(FUZZ_SRC) \
	    $(FUZZ_LDFLAGS) -o $(FUZZ_OUT)/fuzz_spi_frame.exe
	$(call run_fuzz,fuzz_spi_frame)

fuzz-all: fuzz-reg-access fuzz-socket-lifecycle fuzz-poll fuzz-send-recv \
          fuzz-net-config fuzz-context fuzz-spi-frame
