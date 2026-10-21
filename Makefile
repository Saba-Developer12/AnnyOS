SHELL := /bin/sh
.PHONY: all build run clean

all: build

build:
	./scripts/build.sh

run:
	./scripts/run_qemu.sh

clean:
	rm -rf build
