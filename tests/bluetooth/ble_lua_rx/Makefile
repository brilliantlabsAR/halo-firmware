# Host-side reproduction/regression test for the Lua RX GATT write handler
# (modules/halo/src/ble_lua.c). Transcribes the handler with a faithful
# ring_buf stand-in and drives it under ASan/UBSan.
#
#   make run    - production logic; all checks pass (exit 0)
#   make repro  - pre-fix logic; ASan aborts on the length underflow

CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
          -fsanitize=address,undefined -fno-sanitize-recover=all

ble_lua_rx_test: main.c
	$(CC) $(CFLAGS) -o $@ $<

ble_lua_rx_repro: main.c
	$(CC) $(CFLAGS) -DVULNERABLE -o $@ $<

.PHONY: run repro clean
run: ble_lua_rx_test
	./ble_lua_rx_test

# Demonstrates the pre-fix bug: expected to abort under AddressSanitizer,
# so a clean (non-aborting) exit here is itself a failure.
repro: ble_lua_rx_repro
	@if ./ble_lua_rx_repro; then \
		echo "UNEXPECTED: pre-fix build did not abort"; exit 1; \
	else \
		echo "expected AddressSanitizer abort observed"; \
	fi

clean:
	rm -rf ble_lua_rx_test ble_lua_rx_repro *.dSYM
