# flight-terminal

A made-up airline's flight information system with a 1990s airline-terminal
interface, running on an **nRF5340 DK**. Plug in the board, flash it, open a
serial terminal, and you get a green-screen, keyboard-driven system for
browsing the departure schedule, airports, airlines, fleet, crew, passengers,
bookings and baggage.

The records are stored on the DK's 8 MB external QSPI flash using the
[key_value_db](https://github.com/wmilek/key_value_db) storage stack, so they
survive power cycles and reflashing.

## Quick start

You need:
- an nRF5340 DK and a micro-USB cable (use the J-Link USB port, not the nRF USB port)
- Linux with one flashing tool: [nRF Util](https://www.nordicsemi.com/Products/Development-tools/nRF-Util)
  (`nrfutil install device`), `nrfjprog`, or the
  [SEGGER J-Link software](https://www.segger.com/downloads/jlink/)
- a serial terminal: `sudo apt install picocom`

```sh
git clone https://github.com/a-milek/persondb-terminal flight-terminal
cd flight-terminal
./scripts/flash.sh      # writes firmware/flight-terminal-nrf5340dk.hex to the board
./scripts/connect.sh    # opens the terminal UI
```

The repository includes a prebuilt firmware file, so you don't need to compile
anything. To quit picocom, press **Ctrl+A** then **Ctrl+X**.

### First run

The terminal opens straight into the departure schedule. The store starts
empty: press **F** and the board writes the airports, airlines, aircraft and
crew, then the flights with their bookings, passengers and bags. Press **F3**
to stop a fill partway through; everything written so far is kept. Flight
status (ON TIME, BOARDING, DEPARTED, LANDED...) follows the clock in the top
right corner.

## Using it

| Key | Action |
|---|---|
| Up / Down | Move the selection |
| F7 / F8 (or PgUp / PgDn) | Page back / forward |
| Enter | Open the selected item, or run the selected action |
| F3 (or Esc) | Go back or cancel |
| Tab, or 1 to 9 | Switch screens |
| Ctrl+L | Redraw the screen, for example after resizing the window |
| Q | Sign off |

**Departure schedule (FLT100):** every stored flight, in departure order,
opening at the next one due out. **/** finds a flight by number (`XA0421`),
**N** jumps back to the next departure, and Enter opens the flight.

**Flight display (FLT110):** route, aircraft, times in UTC and local time,
delay, gate and seats sold per cabin. F7 / F8 show the previous / next flight,
and **A** / **Z** open the departure / arrival airport.

**Airport directory (FLT200):** every stored airport. **/** finds one by code.
Enter opens the airport display (FLT210), with its departures and arrivals
side by side. Tab switches between the lists, Enter opens a flight, and
F7 / F8 show the previous / next airport.

**System utilities (FLT900):** store statistics, plus fill, verify (compare
every record with the generator) and reset.

The airlines (FLT300), fleet (FLT400), crew (FLT500), passengers (FLT600),
bookings (FLT700) and baggage (FLT800) each have a list and a display screen,
with cross-links between them.

The terminal must be at least 72x20 characters. Larger windows show more rows.

## Troubleshooting

- **Blank screen after connecting:** press **Ctrl+L**. The board draws only what
  has changed, so a terminal that connects late needs a full redraw.
- **Still blank:** the DK has more than one serial port. Try another one:
  `./scripts/connect.sh /dev/ttyACM1`.
- **"No access to /dev/ttyACM0":** add yourself to the port's group (usually
  `dialout`) with `sudo usermod -aG dialout $USER`, then log out and back in.
- **Flashing fails with a protection or readback error:** the chip is locked.
  Run `./scripts/flash.sh --recover`. This erases the internal flash and then
  flashes. The flight store on external flash is not affected.
- **Several boards attached:** pick one with `SERIAL=<J-Link serial> ./scripts/flash.sh`.
- **Windows or macOS:** flash `firmware/flight-terminal-nrf5340dk.hex` with
  nRF Util or the nRF Connect *Programmer* app, then connect with PuTTY (or
  `screen` on macOS) at 115200 baud and press Ctrl+L.

## Running without a board

The terminal also runs in Zephyr's simulator (native_sim), in your terminal.
The only requirement is Docker. The simulated flash is a file, so every
database is a separate file under `dbs/flights/` next to the repository, and a
new name starts empty.

```sh
./scripts/sim.sh demo          # the database in dbs/flights/demo.bin
./scripts/sim.sh --list        # every database
./scripts/mkdb.py big 120      # make a filled database (here 120 flights)
```

`--erase` empties a database before opening it, `--ram` uses a throwaway one,
and `--build` rebuilds the simulator after a code change (it is built on first
use). Press **Q** to sign off, or Ctrl+C to stop.

## Building from source

The only requirement is Docker. The build runs inside Zephyr's CI image and
downloads the ARM toolchain on the first run.

```sh
mkdir flight-ws && cd flight-ws              # the west workspace
git clone https://github.com/a-milek/persondb-terminal flight-terminal
./flight-terminal/scripts/build.sh         # -> build/flight-terminal/zephyr/zephyr.hex
./flight-terminal/scripts/flash.sh build/flight-terminal/zephyr/zephyr.hex
```

The first run puts Zephyr and the required modules (about 1 GB) next to the
repository, which is why it goes in its own workspace folder. Every revision is
pinned in [`west.yml`](west.yml). `./scripts/build.sh --release` also replaces
the firmware in `firmware/`.

If you already have a Zephyr setup with SDK 1.0.1:
`west init -l flight-terminal && west update && west build -b nrf5340dk/nrf5340/cpuapp flight-terminal/app`.

## Layout

```
app/             the Zephyr application
  ui/            the terminal: tui.c draws, ui_tui_flights.c the screens
  scenario/      operations on the store: fill, verify, reset
  flightdb/      the flight database API
  flights/       the generator for the made-up airline
  DESIGN.md      how the storage is laid out, and why
firmware/        prebuilt firmware for the nRF5340 DK
scripts/         build.sh, flash.sh, connect.sh, sim.sh, mkdb.py
west.yml         pinned Zephyr, key_value_db and module revisions
```

This project is derived from `app_cbor_persondb` in
[key_value_db](https://github.com/wmilek/key_value_db). It is licensed under
Apache-2.0.
