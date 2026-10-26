#!/usr/bin/env bash
# Run one of the terminal's applications on native_sim, in this terminal,
# inside the same Docker image as build.sh (the simulator is a 32-bit Linux
# program). Each application is its own build with its own database files:
# the simulated external flash is a file, so each file is a separate
# database, and a new one starts empty, like a fresh DK.
#
#   ./scripts/sim.sh persons work        ../dbs/persons/work.bin
#   ./scripts/sim.sh flights demo        ../dbs/flights/demo.bin
#   ./scripts/sim.sh library city        ../dbs/library/city.bin
#   ./scripts/sim.sh all medium          every database behind the sign-on
#                                        screen, in ../dbs/all/medium.bin
#   ./scripts/sim.sh library             the database named "default"
#   ./scripts/sim.sh library path/x.bin  any file
#   ./scripts/sim.sh library city --erase  empty that database first
#   ./scripts/sim.sh library --ram       a throwaway database, kept in RAM only
#   ./scripts/sim.sh --list              the databases of every application
#   ./scripts/sim.sh library --build     rebuild that application's simulator
#   ./scripts/sim.sh library city -- -stop_at=10   pass options to the simulator
#
# An application's simulator is built on first use (build/sim-<app>, or
# build/sim for "all"). Quit with Q (sign off) or Ctrl+C.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WS=$(dirname "$REPO")
NAME=$(basename "$REPO")
DBS=$WS/dbs
APPS="persons flights library all"
SDK_VER=1.0.1
# The image the simulator is built with, as in build.sh.
IMAGE=ghcr.io/zephyrproject-rtos/ci-base@sha256:f09a9642ff7558a21f5d0a2a720844c228a73b0d3d3e6112f1cfd7f6d8970bae

usage() {
	sed -n '2,/^set -e/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//' >&2
	exit 1
}

APP=
DB=default
BUILD=false
FLASH_ARGS=()
while [ $# -gt 0 ]; do
	arg=$1
	shift
	case "$arg" in
	--) break ;;
	--erase) FLASH_ARGS+=(-flash_erase) ;;
	--ram) DB= FLASH_ARGS+=(-flash_in_ram) ;;
	--build) BUILD=true ;;
	--list)
		for a in $APPS; do
			NAMES=$(ls "$DBS/$a" 2>/dev/null | sed -n 's/\.bin$//p' | tr '\n' ' ' || true)
			printf '%-8s %s\n' "$a" "${NAMES:-(none)}"
		done
		exit 0 ;;
	-h | --help) usage ;;
	-*) echo "sim.sh: unknown option $arg" >&2; exit 1 ;;
	*)
		if [ -z "$APP" ]; then
			case " $APPS " in
			*" $arg "*) APP=$arg ;;
			*) echo "sim.sh: no application '$arg'; pick one of: $APPS" >&2; exit 1 ;;
			esac
		else
			DB=$arg
		fi ;;
	esac
done
[ -n "$APP" ] || usage

command -v docker >/dev/null || { echo "sim.sh: Docker is required" >&2; exit 1; }

if [ "$APP" = all ]; then
	BUILD_DIR=build/sim
	KCONFIG=-DCONFIG_APP_CBOR_PERSONDB_TUI_ALL=y
else
	BUILD_DIR=build/sim-$APP
	KCONFIG=-DCONFIG_APP_CBOR_PERSONDB_TUI_${APP^^}=y
fi
EXE=$BUILD_DIR/zephyr/zephyr.exe

if $BUILD || [ ! -f "$WS/$EXE" ]; then
	echo "Building the $APP simulator in $WS/$BUILD_DIR..."
	docker run --rm --network host -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$WS":/ws -w /ws \
		"$IMAGE" bash -c "ZEPHYR_SDK_INSTALL_DIR=/ws/.zephyr-sdk/zephyr-sdk-$SDK_VER \
		west build -p auto -b native_sim -d $BUILD_DIR $NAME/app -- $KCONFIG" |
		grep -E 'warning:|error:|FAILED|Linking C executable' || true
	[ -f "$WS/$EXE" ] || { echo "sim.sh: the build failed" >&2; exit 1; }
fi

# A plain name lives in ../dbs/<app>; anything with a slash or .bin is a path.
# The file's directory is mounted at /db, so it can be anywhere on the host.
MOUNTS=(-v "$WS":/ws)
if [ -n "$DB" ]; then
	case "$DB" in
	*/* | *.bin) FILE=$(realpath -m "$DB") ;;
	*) FILE=$DBS/$APP/$DB.bin ;;
	esac
	mkdir -p "$(dirname "$FILE")"
	MOUNTS+=(-v "$(dirname "$FILE")":/db)
	FLASH_ARGS+=("-flash=/db/$(basename "$FILE")")
	echo "Database: $FILE"
fi

# The simulator reads the UART from stdin but leaves the terminal in line
# mode; switch it to one key at a time without echo, as picocom does.
exec docker run --rm -it -u "$(id -u):$(id -g)" "${MOUNTS[@]}" -w /ws "$IMAGE" \
	sh -c 'stty -icanon -echo -icrnl min 1; exec "$@"' sh "$EXE" "${FLASH_ARGS[@]}" "$@"
