SHELL := /bin/sh
.PHONY: all build run curl curl-host clean

all: build

build:
	bash ./scripts/build.sh

run: build
	bash ./scripts/run_qemu.sh

curl-host:
	bash ./scripts/build_curl_host.sh

curl: curl-host

clean:
	rm -rf build
