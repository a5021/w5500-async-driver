# W5500 async driver — host build (core library + unit tests)
#
#   make test   build and run the unit tests against the W5500 emulator
#   make lib    build build/libw5500.a (core only, for host-side use)
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
