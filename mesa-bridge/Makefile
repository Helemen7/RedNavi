# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# The bridge and its test, built on their own for x86_64 macOS (the machine with the card). Inside a Mesa tree only src/ac_apple_iokit.c goes in.
CC ?= clang
CFLAGS := -O1 -Wall -Wextra -Wno-unused-parameter -arch x86_64 -mmacosx-version-min=12 -Iinclude -I../RedNavi
LIBS := -framework IOKit -framework CoreFoundation

test: bridge_test
bridge_test: test/bridge_test.c src/ac_apple_iokit.c include/*.h ../RedNavi/rn_uapi.h ../RedNavi/rn_drm_uapi.h
	$(CC) $(CFLAGS) test/bridge_test.c src/ac_apple_iokit.c -o $@ $(LIBS)
clean:
	rm -f bridge_test
.PHONY: test clean
