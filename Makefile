CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
PREFIX ?= $(HOME)/.local

SRC := $(wildcard src/*.c src/targets/*.c)
HDR := $(wildcard src/*.h src/targets/*.h)
OBJ := $(SRC:src/%.c=build/%.o)

.PHONY: all test install clean

all: build/wind

build/wind: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

build/%.o: src/%.c $(HDR) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build/targets

test: build/wind
	sh tests/run.sh
	python3 tests/lsp.py

# The libraries are installed where the compiler looks for them beside
# itself: share/wind/libs under the same prefix.
install: build/wind
	install -Dm755 build/wind $(DESTDIR)$(PREFIX)/bin/wind
	rm -rf $(DESTDIR)$(PREFIX)/share/wind/libs
	mkdir -p $(DESTDIR)$(PREFIX)/share/wind
	cp -R libs $(DESTDIR)$(PREFIX)/share/wind/libs

clean:
	rm -rf build
