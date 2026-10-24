#!/usr/bin/env bash
# Open the terminal UI on an nRF5340 DK: a serial terminal on the board's
# USB port at 115200 baud, then a redraw request so the screen appears even
# if the board started long before.
#
#   ./scripts/connect.sh                 the DK's first J-Link serial port
#   ./scripts/connect.sh /dev/ttyACM1    a port of your choice
#
# picocom: quit with Ctrl+A then Ctrl+X. screen: Ctrl+A then K.
set -euo pipefail

PORT=${1:-}
if [ -z "$PORT" ]; then
	# The J-Link on the DK has two or three serial ports; the application
	# core's console is on its first interface.
	PORT=$(ls /dev/serial/by-id/*SEGGER*-if00 2>/dev/null | head -1 || true)
	PORT=${PORT:-$(ls /dev/ttyACM* 2>/dev/null | head -1 || true)}
fi
if [ -z "$PORT" ] || [ ! -e "$PORT" ]; then
	echo "connect.sh: no DK serial port found. Is the board plugged in and switched on?" >&2
	exit 1
fi
if [ ! -r "$PORT" ] || [ ! -w "$PORT" ]; then
	echo "connect.sh: no access to $PORT. Add yourself to its group, then log in again:" >&2
	echo "  sudo usermod -aG $(stat -c %G "$PORT") $USER" >&2
	exit 1
fi

echo "Opening $PORT. If the screen stays blank, press Ctrl+L."
if command -v picocom >/dev/null; then
	exec picocom -q -b 115200 --initstring $'\x0c' "$PORT"
elif command -v screen >/dev/null; then
	exec screen "$PORT" 115200
elif python3 -c 'import serial' 2>/dev/null; then
	exec python3 -m serial.tools.miniterm --raw "$PORT" 115200
else
	echo "connect.sh: install picocom (sudo apt install picocom), screen or pyserial" >&2
	exit 1
fi
