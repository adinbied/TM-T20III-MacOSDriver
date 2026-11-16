# TM-T20III utility - C core, CLI and tests.
#   make test     protocol tests against the simulator (Linux or macOS)
#   make tmutil   CLI (TCP everywhere; USB on macOS)
#   make tmqueue  Printing Preferences CLI (queue PPD settings)
#   make app      SwiftUI app bundle (macOS, needs Xcode toolchain)

CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
INC     = -ISources/TMCore/include
CORE    = Sources/TMCore/tmcore.c Sources/TMCore/tmlink_posix.c Sources/TMCore/tmdriver.c

UNAME := $(shell uname -s)

# Wipe build/ if it was produced on a different OS/CPU (copied tree).
PLATFORM := $(shell uname -sm | tr ' ' '-')
$(shell [ -f build/.platform-$(PLATFORM) ] || { rm -rf build; mkdir -p build; touch build/.platform-$(PLATFORM); })

ifeq ($(UNAME),Darwin)
  CORE   += Sources/TMCore/tmlink_usb_darwin.m
  ARCHF   = -arch arm64 -arch x86_64 -mmacosx-version-min=11.0
  PLAT    = $(ARCHF) -fobjc-arc \
            -framework Foundation -framework IOKit -framework IOUSBHost
  CFLAGS := $(filter-out -D_POSIX_C_SOURCE=200809L,$(CFLAGS))
endif

all: build/tmutil build/tmqueue

build:
	mkdir -p build

build/tmutil: cli/tmutil.c $(CORE) Sources/TMCore/include/tmcore.h | build
	$(CC) $(CFLAGS) $(INC) $(PLAT) -o $@ cli/tmutil.c $(CORE)

tmutil: build/tmutil

# Printing Preferences from the shell (links libcups for queue PPD access).
build/tmqueue: cli/tmqueue.c Sources/TMCore/tmdriver.c Sources/TMCore/include/tmdriver.h | build
	$(CC) $(CFLAGS) $(INC) $(ARCHF) -Wno-deprecated-declarations \
	    -o $@ cli/tmqueue.c Sources/TMCore/tmdriver.c -lcups

tmqueue: build/tmqueue

build/tmdriver_test: tests/tmdriver_test.c Sources/TMCore/tmdriver.c | build
	$(CC) $(CFLAGS) $(INC) -o $@ tests/tmdriver_test.c Sources/TMCore/tmdriver.c

build/tmcore_test: tests/tmcore_test.c tests/tmsim.c $(CORE) | build
	$(CC) $(CFLAGS) $(INC) -Itests $(PLAT) -o $@ tests/tmcore_test.c tests/tmsim.c $(CORE) -lpthread

build/simserver: tests/simserver.c tests/tmsim.c | build
	$(CC) $(CFLAGS) -Itests -o $@ tests/simserver.c tests/tmsim.c -lpthread

test: build/tmcore_test build/tmdriver_test
	./build/tmcore_test
	./build/tmdriver_test ../ppd/EpsonTMT20III.ppd

# Try the CLI without a printer: make sim, then build/tmutil --tcp 127.0.0.1:19100 get
sim: build/simserver build/tmutil
	./build/simserver 19100

app:
	./scripts/make-app.sh

clean:
	rm -rf build .build TMUtility.app "TM-T20III Utility.app"

.PHONY: all tmutil tmqueue test app clean
