# Tier 1 host test harness for ble_findmy domain code. Plain gcc, no Flipper
# SDK -- see ../CLAUDE.md "Two-tier testing strategy". Run from this
# directory: `make test`.

CC ?= gcc

BUILD_DIR := build

# Test 1: ble_findmy payload
BLE_DIR := ../universal_toolkit/modules/ble_findmy
BLE_BIN := $(BUILD_DIR)/test_findmy_payload
BLE_SRCS := test_findmy_payload.c $(BLE_DIR)/findmy_payload.c
BLE_DEPS := framework/test_framework.h $(BLE_DIR)/findmy_payload.h

# Tests 2 & 3: universal_card_reader save/load/emulation fidelity
CARD_DIR := ../universal_toolkit/universal_card_reader
CARD_CFLAGS := -I$(CARD_DIR)
CARD_DEPS := framework/test_framework.h $(CARD_DIR)/emulation_state.h test_nfc_file_parse.h

FIDE_BIN := $(BUILD_DIR)/test_emulation_fidelity
FIDE_SRCS := test_emulation_fidelity.c test_nfc_file_parse.c

ROUND_BIN := $(BUILD_DIR)/test_save_load_roundtrip
ROUND_SRCS := test_save_load_roundtrip.c test_nfc_file_parse.c

EMVEMU_BIN := $(BUILD_DIR)/test_emv_emulate
EMVEMU_SRCS := test_emv_emulate.c $(CARD_DIR)/emv_emulate.c
EMVEMU_DEPS := framework/test_framework.h $(CARD_DIR)/emv_emulate.h

CFLAGS := -std=c11 -Wall -Wextra -Werror -I$(BLE_DIR) $(CARD_CFLAGS)

.PHONY: test clean

test: $(BLE_BIN) $(FIDE_BIN) $(ROUND_BIN) $(EMVEMU_BIN)
	./$(BLE_BIN)
	./$(FIDE_BIN)
	./$(ROUND_BIN)
	./$(EMVEMU_BIN)

$(BLE_BIN): $(BLE_SRCS) $(BLE_DEPS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(BLE_SRCS)

$(FIDE_BIN): $(FIDE_SRCS) $(CARD_DEPS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(FIDE_SRCS)

$(ROUND_BIN): $(ROUND_SRCS) $(CARD_DEPS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(ROUND_SRCS)

$(EMVEMU_BIN): $(EMVEMU_SRCS) $(EMVEMU_DEPS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(EMVEMU_SRCS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
