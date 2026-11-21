# Builds the user-space driver library and tools. Requires libusb (brew install libusb).
BREW   ?= $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
CFLAGS ?= -O2 -g -Wall -Wextra
CFLAGS += -std=c11 -Isrc/libevc -I$(BREW)/include/libusb-1.0
LDLIBS  = -L$(BREW)/lib -lusb-1.0

all: out/evc-capture out/lsdesc out/usbreset

out/libevc.a: src/libevc/evc.c src/libevc/evc.h
	@mkdir -p out
	$(CC) $(CFLAGS) -c src/libevc/evc.c -o out/evc.o
	ar rcs $@ out/evc.o

out/evc-capture: src/tools/evc-capture.c out/libevc.a
	$(CC) $(CFLAGS) $< out/libevc.a $(LDLIBS) -o $@

out/lsdesc: tools/usb/lsdesc.c
	@mkdir -p out
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@

out/usbreset: tools/usb/usbreset.c
	@mkdir -p out
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@

clean:
	rm -rf out

.PHONY: all clean
