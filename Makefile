SHELL := /bin/sh
.PHONY: all build run clean

all: build

build:
	bash ./scripts/build.sh

run: build
	bash ./scripts/run_qemu.sh

clean:
	rm -rf build
