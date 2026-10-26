#!/usr/bin/env python3
"""Make a filled database for one of the simulator applications.

    ./scripts/mkdb.py persons work 500     500 people in ../dbs/persons/work.bin
    ./scripts/mkdb.py flights demo 120     120 flights (and every airport)
    ./scripts/mkdb.py library city 300     300 books, with their loans

It runs the application through sim.sh and types what an operator would: F,
the count, Enter. An existing database of that name is emptied first.
"""
import os
import pty
import re
import select
import sys
import time

APPS = {
    # screen code, fill result line
    "persons": ("PDB100", r"(\d+) OF \d+ PERSONS NOW IN THE STORE"),
    "flights": ("FLT100", r"STORE HOLDS (\d+) AIRPORTS, (\d+) FLIGHTS"),
    "library": ("LIB100", r"STORE HOLDS (\d+) BOOKS, (\d+) ON LOAN"),
}
SIM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sim.sh")
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b[78]")


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in APPS or not sys.argv[3].isdigit():
        sys.exit(__doc__.strip())
    app, name, count = sys.argv[1], sys.argv[2], sys.argv[3]
    screen, result = APPS[app]

    pid, fd = pty.fork()
    if pid == 0:
        os.execv(SIM, [SIM, app, name, "--erase"])
    seen = b""

    def wait(pattern, timeout=600):
        nonlocal seen
        start = len(seen)
        end = time.time() + timeout
        while time.time() < end:
            ready, _, _ = select.select([fd], [], [], 0.2)
            if ready:
                try:
                    seen += os.read(fd, 65536)
                except OSError:
                    break
            text = re.sub(r" +", " ", ANSI.sub(b" ", seen[start:]).decode("latin1"))
            m = re.search(pattern, text)
            if m:
                return m
        sys.exit(f"mkdb: {app} {name}: no {pattern!r} on screen")

    def key(keys, pattern=None, timeout=600):
        os.write(fd, keys.encode())
        time.sleep(0.3)
        return wait(pattern, timeout) if pattern else None

    wait(screen)
    key("F", "ENTER=ACCEPT")
    # The prompt holds a default: delete it, then type the count.
    m = key("\x7f" * 10 + count + "\r", result)
    print(f"{app} {name}: {m.group(0).lower()}")
    key("\r")
    os.write(fd, b"\x03")
    time.sleep(1)


if __name__ == "__main__":
    main()
