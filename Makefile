CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
PREFIX ?= $(HOME)/.local

SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)
OBJ := $(SRC:src/%.c=build/%.o)

.PHONY: all test install clean

all: build/wind

build/wind: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

build/%.o: src/%.c $(HDR) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

test: build/wind
	sh tests/run.sh
	python3 tests/lsp.py

install: build/wind
	install -Dm755 build/wind $(DESTDIR)$(PREFIX)/bin/wind

clean:
	rm -rf build
