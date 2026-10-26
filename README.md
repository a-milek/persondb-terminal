# persondb-terminal

An access-control database with a 1990s airline-terminal interface, running on
an **nRF5340 DK**. Plug in the board, flash it, open a serial terminal, and you
get a green-screen, keyboard-driven system for browsing people, granting door
permissions, assigning cards and checking access.

```
 PDB100  PERSON DIRECTORY           PERSONDB ACCESS CONTROL       28DEC27 1320Z
-- 1 PERSONS -- 2 ACCESS -- 3 UTILITIES ----------------------------------------
     NBR  ID       NAME                   DEPT          TITLE         PERM  CARD
  000000  100000   OKAFOR-0000/CIRA       SUPPORT       ENGINEER        17     2
  000001  100001   KOVAC-0001/EVE         SECURITY      CONTRACTOR      22     2
  000002  100002   HADDAD-0002/OONA       FACILITIES    SPECIALIST      22     3
  ...
 >                                                                LINE 1 OF 20
 ENTER=DISPLAY  F7=BKWD  F8=FWD  /=LOCATE  F=FILL  TAB=NEXT SCREEN  Q=SIGN OFF
 1A  ONLINE                                   REV 0  20/8000 PERSONS  LU PDB01
```

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
git clone https://github.com/a-milek/persondb-terminal
cd persondb-terminal
./scripts/flash.sh      # writes firmware/persondb-terminal-nrf5340dk.hex to the board
./scripts/connect.sh    # opens the terminal UI
```

The repository includes a prebuilt firmware file, so you don't need to compile
anything. To quit picocom, press **Ctrl+A** then **Ctrl+X**.

### First run

The terminal opens on a sign-on screen (SON100) that lists its two databases,
the person database and the flight information, with what each store holds.
Pick one with the arrow keys or **1** / **2** and press Enter. Press **S** in
either one to come back and switch; nothing is lost by switching.

The person store starts empty. Press **F**, enter how many people to generate, and
press Enter. The external flash writes at roughly one person per second, so
200 people take about 3 minutes. Press **F3** to stop a fill partway through.
Everything written so far is kept.

## Using it

| Key | Action |
|---|---|
| Up / Down | Move the selection |
| F7 / F8 (or PgUp / PgDn) | Page back / forward |
| Enter | Open the selected item, or run the selected action |
| F3 (or Esc) | Go back or cancel |
| Tab, or 1 / 2 / 3 | Switch screens: Persons, Access, Utilities |
| S | Back to the sign-on screen, to switch databases |
| Ctrl+L | Redraw the screen, for example after resizing the window |
| Q | Sign off |

**Persons (PDB100):** a list of everyone. Press **/** to jump to a record
number and **F** to generate more people. Press Enter to open a record.

**Person record (PDB110):** details, with permissions and cards side by side.
Tab switches between the two lists.
- **G** grants a permission from a list that marks the ones already held.
- **D** deletes the selected permission or card.
- **A** assigns a new card, as 14 hex digits.
- **C** checks access with the selected card.
- F7 / F8 show the previous / next person.

**Access verification (PDB200):** enter a card and a door permission to see
GRANTED, DENIED, EXPIRED or UNKNOWN CARD, the time taken, and recent checks.

**System utilities (PDB900):** store statistics, plus fill, verify, mutate,
benchmark and reset (reset erases every record).

### Flight database

The second database on the sign-on screen is a made-up airline, with
fictional airports and a daily schedule of departures. It is a separate store
on the same external flash as the persons, under its own registry key, and
the same keys work in it. **Reset store** in the person database erases the
whole flash, flights included. **Reset flight store** erases only the flights.

The screens show what the flight store holds. It starts empty: press **F** and
the board writes 20 airports and up to 240 flights to flash. Flight status
(ON TIME, BOARDING, DEPARTED, LANDED...) follows the clock in the top right
corner.

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
  flashes. The stored people on external flash are not affected.
- **Several boards attached:** pick one with `SERIAL=<J-Link serial> ./scripts/flash.sh`.
- **Windows or macOS:** flash `firmware/persondb-terminal-nrf5340dk.hex` with
  nRF Util or the nRF Connect *Programmer* app, then connect with PuTTY (or
  `screen` on macOS) at 115200 baud and press Ctrl+L.

## Building from source

The only requirement is Docker. The build runs inside Zephyr's CI image and
downloads the ARM toolchain on the first run.

```sh
mkdir persondb-ws && cd persondb-ws          # the west workspace
git clone https://github.com/a-milek/persondb-terminal
./persondb-terminal/scripts/build.sh         # -> build/persondb-terminal/zephyr/zephyr.hex
./persondb-terminal/scripts/flash.sh build/persondb-terminal/zephyr/zephyr.hex
```

The first run puts Zephyr and the required modules (about 1 GB) next to the
repository, which is why it goes in its own workspace folder. Every revision is
pinned in [`west.yml`](west.yml). `./scripts/build.sh --release` also replaces
the firmware in `firmware/`.

If you already have a Zephyr setup with SDK 1.0.1:
`west init -l persondb-terminal && west update && west build -b nrf5340dk/nrf5340/cpuapp persondb-terminal/app`.

## Layout

```
app/             the Zephyr application
  ui/            the terminal: tui.c draws, ui_tui_main.c is the sign-on
                 screen, ui_tui.c the persons, ui_tui_flights.c the flights
  scenario/      operations on the stores: fill, verify, benchmark
  persondb/      the person and credential database API
  dataset/       the generator for synthetic people
  flightdb/      the flight and airport database API
  flights/       the generator for the made-up airline schedule
  DESIGN.md      how the storage is laid out, and why
firmware/        prebuilt firmware for the nRF5340 DK
scripts/         build.sh, flash.sh, connect.sh
west.yml         pinned Zephyr, key_value_db and module revisions
```

The app also has two other frontends: an interactive shell
(`-DCONFIG_APP_CBOR_PERSONDB_FRONTEND_SHELL=y`) and an unattended benchmark
(`-DCONFIG_APP_CBOR_PERSONDB_FRONTEND_BENCH=y`).

This project is derived from `app_cbor_persondb` in
[key_value_db](https://github.com/wmilek/key_value_db). It is licensed under
Apache-2.0.
