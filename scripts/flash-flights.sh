#!/usr/bin/env bash
# Flash the local build of ./scripts/build.sh onto the bench DK on
# /dev/ttyACM3.
#
#   ./scripts/flash-flights.sh
#   SERIAL=1050091053 ./scripts/flash-flights.sh   another board
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export SERIAL=${SERIAL:-960115021}
exec "$REPO/scripts/flash.sh" "$(dirname "$REPO")/build/flight-terminal/zephyr/zephyr.hex" "$@"
