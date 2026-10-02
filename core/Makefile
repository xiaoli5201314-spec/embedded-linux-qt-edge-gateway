# ---------------------------------------------------------------------------
# Gateway core library - host build (unit tests) and cross build (target)
#
#   make            # build the host test binary and run it
#   make test       # run the unit tests
#   make cross      # cross compile the static library for the T113 target
#   make clean
# ---------------------------------------------------------------------------

CC          ?= gcc
CROSS_CC    ?= arm-linux-gnueabihf-gcc
CFLAGS      ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
INC          = -Iinclude
BUILD        = build

SRCS         = src/crc16.c src/modbus_rtu.c src/offline_cache.c \
               src/reconnect_fsm.c src/register_map.c src/rs485_port.c
TEST_SRC     = test/test_core.c test/test_rs485_port.c

OBJS         = $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all test cross clean

all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(BUILD)/test_core: $(OBJS) $(TEST_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(TEST_SRC) $(OBJS) -lm -o $@

test: $(BUILD)/test_core
	@echo "--- running unit tests ---"
	@./$(BUILD)/test_core

# Static library for the target board.
cross: | $(BUILD)
	$(CROSS_CC) $(CFLAGS) $(INC) -c $(SRCS)
	@mkdir -p $(BUILD)/cross
	@mv -f *.o $(BUILD)/cross/ 2>/dev/null || true
	$(CROSS_CC) -c $(CFLAGS) $(INC) src/*.c
	ar rcs $(BUILD)/libgateway_core.a *.o
	@mv -f *.o $(BUILD)/cross/ 2>/dev/null || true
	@echo "built $(BUILD)/libgateway_core.a"

clean:
	@rm -rf $(BUILD)
	@echo "cleaned"
