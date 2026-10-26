#!/usr/bin/env bash
# Flash the original key_value_db person database (shell frontend) onto the
# DK on /dev/ttyACM3, from its build in ~/kvdb_ws/build_persondb.
#
#   ./scripts/flash-persondb.sh
#   SERIAL=1050091053 ./scripts/flash-persondb.sh  another board
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export SERIAL=${SERIAL:-960115021}
exec "$REPO/scripts/flash.sh" "$HOME/kvdb_ws/build_persondb/zephyr/zephyr.hex" "$@"
