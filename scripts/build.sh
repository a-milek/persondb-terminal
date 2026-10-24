#!/usr/bin/env bash
# Build the firmware from source. Needs only Docker: west, CMake and the
# rest come from Zephyr's CI image, and the ARM toolchain (~165 MB) is
# downloaded once into the workspace.
#
# The workspace is the directory that holds this repository; west clones
# Zephyr and the modules next to it on the first run (~1 GB).
#
#   ./scripts/build.sh            -> ../build/persondb-terminal/zephyr/zephyr.hex
#   ./scripts/build.sh --release  ... and copy it to firmware/
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WS=$(dirname "$REPO")
NAME=$(basename "$REPO")
BOARD=nrf5340dk/nrf5340/cpuapp
SDK_VER=1.0.1
# The image the firmware in firmware/ was built with.
IMAGE=ghcr.io/zephyrproject-rtos/ci-base@sha256:f09a9642ff7558a21f5d0a2a720844c228a73b0d3d3e6112f1cfd7f6d8970bae

command -v docker >/dev/null || { echo "build.sh: Docker is required" >&2; exit 1; }

docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$WS":/ws -w /ws "$IMAGE" \
	bash -euo pipefail -c "
	if [ ! -d .west ]; then
		west init -l '$NAME'
	fi
	west update --narrow -o=--depth=1

	sdk=/ws/.zephyr-sdk/zephyr-sdk-$SDK_VER
	if [ ! -x \$sdk/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc ]; then
		echo 'Downloading the Zephyr SDK $SDK_VER (minimal + ARM toolchain)...'
		url=https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v$SDK_VER
		mkdir -p /ws/.zephyr-sdk \$sdk/gnu
		wget -qO- \$url/zephyr-sdk-${SDK_VER}_linux-x86_64_minimal.tar.xz | tar xJ -C /ws/.zephyr-sdk
		wget -qO- \$url/toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz | tar xJ -C \$sdk/gnu
	fi

	ZEPHYR_SDK_INSTALL_DIR=\$sdk west build -p auto -b $BOARD -d build/$NAME $NAME/app
	"

HEX="$WS/build/$NAME/zephyr/zephyr.hex"
if [ "${1:-}" = "--release" ]; then
	cp "$HEX" "$REPO/firmware/persondb-terminal-nrf5340dk.hex"
	HEX="$REPO/firmware/persondb-terminal-nrf5340dk.hex"
fi
echo
echo "Firmware: $HEX"
echo "Flash it: ./scripts/flash.sh $HEX"
