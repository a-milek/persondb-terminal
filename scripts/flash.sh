#!/usr/bin/env bash
# Flash the firmware onto an nRF5340 DK over its on-board J-Link, with
# whichever Nordic/SEGGER tool is installed: nrfutil (with its "device"
# command), nrfjprog or JLinkExe.
#
#   ./scripts/flash.sh             the prebuilt firmware in firmware/
#   ./scripts/flash.sh FILE.hex    another build
#   ./scripts/flash.sh --recover   unlock a protected chip (erases it), then flash
#
# With more than one board attached, choose one: SERIAL=1050123456 ./scripts/flash.sh
#
# Only the internal flash is touched: the person and flight stores live on
# the DK's external QSPI flash and survive reflashing.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
RECOVER=0
HEX="$REPO/firmware/flight-terminal-nrf5340dk.hex"
for a in "$@"; do
	case "$a" in
	--recover) RECOVER=1 ;;
	-h|--help) sed -n 2,14p "$0"; exit 0 ;;
	*) HEX=$a ;;
	esac
done
[ -f "$HEX" ] || { echo "flash.sh: no such file: $HEX" >&2; exit 1; }
SERIAL=${SERIAL:-}

if command -v nrfutil >/dev/null && nrfutil device --version >/dev/null 2>&1; then
	sel=()
	[ -n "$SERIAL" ] && sel=(--serial-number "$SERIAL")
	if [ $RECOVER = 1 ]; then
		nrfutil device recover "${sel[@]}"
	fi
	nrfutil device program "${sel[@]}" --firmware "$HEX" --core Application \
		--options chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE,verify=VERIFY_READ
	nrfutil device reset "${sel[@]}"
elif command -v nrfjprog >/dev/null; then
	sel=()
	[ -n "$SERIAL" ] && sel=(--snr "$SERIAL")
	if [ $RECOVER = 1 ]; then
		nrfjprog -f NRF53 --coprocessor CP_NETWORK --recover "${sel[@]}"
		nrfjprog -f NRF53 --coprocessor CP_APPLICATION --recover "${sel[@]}"
	fi
	nrfjprog -f NRF53 --coprocessor CP_APPLICATION --program "$HEX" --sectorerase \
		--verify "${sel[@]}"
	nrfjprog -f NRF53 --reset "${sel[@]}"
elif command -v JLinkExe >/dev/null; then
	[ $RECOVER = 1 ] && echo "flash.sh: --recover needs nrfutil or nrfjprog" >&2 && exit 1
	script=$(mktemp)
	trap 'rm -f "$script"' EXIT
	printf 'r\nloadfile %s\nr\ng\nexit\n' "$HEX" > "$script"
	sel=()
	[ -n "$SERIAL" ] && sel=(-USB "$SERIAL")
	JLinkExe "${sel[@]}" -device nRF5340_xxAA_APP -if SWD -speed 4000 -autoconnect 1 \
		-NoGui 1 -ExitOnError 1 -CommanderScript "$script"
else
	cat >&2 <<'MSG'
flash.sh: no flashing tool found. Install one of:
  - nRF Util:  https://www.nordicsemi.com/Products/Development-tools/nRF-Util
               then: nrfutil install device
  - SEGGER J-Link Software (gives JLinkExe): https://www.segger.com/downloads/jlink/
MSG
	exit 1
fi

echo
echo "Flashed. Connect to it: ./scripts/connect.sh"
