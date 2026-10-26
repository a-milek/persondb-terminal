#!/usr/bin/env bash
# Flash the flights-only terminal (no sign-on screen) onto the DK on
# /dev/ttyACM3, from the local build of ./scripts/build.sh flights.
#
#   ./scripts/flash-flights.sh
#   SERIAL=1050091053 ./scripts/flash-flights.sh   another board
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export SERIAL=${SERIAL:-960115021}
exec "$REPO/scripts/flash.sh" "$(dirname "$REPO")/build/flight-terminal-flights/zephyr/zephyr.hex" "$@"
