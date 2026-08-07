# Tier 1 host test harness for ble_findmy domain code. Plain gcc, no Flipper
# SDK -- see ../CLAUDE.md "Two-tier testing strategy". Run from this
# directory: `make test`.

CC ?= gcc
MODULE_DIR := ../universal_toolkit/modules/ble_findmy
CFLAGS := -std=c11 -Wall -Wextra -Werror -I$(MODULE_DIR)
BUILD_DIR := build
BIN := $(BUILD_DIR)/test_findmy_payload

SRCS := test_findmy_payload.c $(MODULE_DIR)/findmy_payload.c
DEPS := framework/test_framework.h $(MODULE_DIR)/findmy_payload.h

.PHONY: test clean

test: $(BIN)
	./$(BIN)

$(BIN): $(SRCS) $(DEPS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
