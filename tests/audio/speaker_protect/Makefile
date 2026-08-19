# Host-side characterisation tests for the speaker current-protection chain.
# Compiles the exact production DSP source (no Zephyr dependencies) with
# ASan/UBSan and verifies frequency response, limiter behaviour and safety
# properties. Run: make run

DSP_SRC := ../../../drivers/audio/max98357a/speaker_protect.c
CFLAGS  := -std=c99 -O1 -g -Wall -Wextra -Werror \
           -fsanitize=address,undefined -fno-sanitize-recover=all \
           -I../../../drivers/audio/max98357a

test_speaker_protect: main.c $(DSP_SRC)
	$(CC) $(CFLAGS) -o $@ $^ -lm

.PHONY: run clean
run: test_speaker_protect
	./test_speaker_protect

clean:
	rm -f test_speaker_protect
