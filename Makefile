# Epson TM-T20III CUPS driver for macOS (Apple Silicon + Intel universal)
#
#   make            build build/rastertotmt20 (universal)
#   make test       host self-test of the encoder (no CUPS needed)
#   make filter-test  real filter + PPD end-to-end (needs libcups)
#   sudo make install / uninstall
#   make installer    signed + notarized dist/*.pkg (scripts/make-installer.sh)

CC       ?= clang
ARCHS    ?= -arch arm64 -arch x86_64
MINOS    ?= -mmacosx-version-min=11.0
CFLAGS   ?= -O2 -Wall -Wextra
DEST     ?= /Library/Printers/Epson/TMT20III
PPDDIR   ?= /Library/Printers/PPDs/Contents/Resources

UNAME := $(shell uname -s)

# build/ may have been copied from another machine (e.g. Linux test builds);
# wipe it when it was made on a different OS/CPU so make never skips a rebuild.
PLATFORM := $(shell uname -sm | tr ' ' '-')
$(shell [ -f build/.platform-$(PLATFORM) ] || { rm -rf build; mkdir -p build; touch build/.platform-$(PLATFORM); })

ifeq ($(UNAME),Darwin)
  PLAT_CFLAGS = $(ARCHS) $(MINOS)
else
  PLAT_CFLAGS =
endif

all: build/rastertotmt20

build:
	mkdir -p build

build/rastertotmt20: src/rastertotmt20.c src/tmt20.c src/tmt20.h | build
	$(CC) $(CFLAGS) $(PLAT_CFLAGS) -Wno-deprecated-declarations \
	    -o $@ src/rastertotmt20.c src/tmt20.c -lcups -lz -lm

# Host test: no CUPS, builds anywhere with a C compiler + zlib.
build/tmt20_test: tests/tmt20_test.c src/tmt20.c src/tmt20.h | build
	$(CC) $(CFLAGS) -Isrc -o $@ tests/tmt20_test.c src/tmt20.c -lz -lm

# End-to-end: real filter + PPD on generated CUPS raster (needs libcups).
build/mkraster: tests/mkraster.c | build
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -o $@ tests/mkraster.c -lcups -lm

filter-test: build/rastertotmt20 build/mkraster
	python3 tests/filter_test.py

test: build/tmt20_test
	./build/tmt20_test build/test_out.bin
	python3 tests/escpos_decode.py build/test_out.bin --pbm build/test_out.pbm --expect-rows 2436 \
	    --compare build/test_out.bin.expected.pbm

install: build/rastertotmt20
	install -d -o root -g wheel -m 755 $(DEST)
	install -o root -g wheel -m 755 build/rastertotmt20 $(DEST)/rastertotmt20
	install -o root -g wheel -m 755 scripts/add-printer.sh scripts/uninstall.sh $(DEST)/
	install -o root -g wheel -m 644 ppd/EpsonTMT20III.ppd $(PPDDIR)/EpsonTMT20III.ppd
	@echo "Installed. Add the printer with:  ./scripts/add-printer.sh"

uninstall:
	rm -f $(DEST)/rastertotmt20 $(DEST)/add-printer.sh $(DEST)/uninstall.sh $(PPDDIR)/EpsonTMT20III.ppd
	-rmdir $(DEST)

installer:
	./scripts/make-installer.sh

clean:
	rm -rf build

.PHONY: all test filter-test install uninstall installer clean
