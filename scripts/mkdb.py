#!/usr/bin/env python3
"""Make a filled flight database for the simulator.

    ./scripts/mkdb.py demo 120     120 flights (and every airport) in
                                   ../dbs/flights/demo.bin

It runs the terminal through sim.sh and types what an operator would: F,
the count, Enter. An existing database of that name is emptied first.
"""
import os
import pty
import re
import select
import sys
import time

# The screen the terminal opens on, and the line a finished fill shows.
SCREEN = "FLT100"
RESULT = r"STORE: (\d+) FLIGHTS, (\d+) BOOKINGS, (\d+) BAGS"
SIM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sim.sh")
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b[78]")


def main():
    if len(sys.argv) != 3 or not sys.argv[2].isdigit():
        sys.exit(__doc__.strip())
    name, count = sys.argv[1], sys.argv[2]

    pid, fd = pty.fork()
    if pid == 0:
        os.execv(SIM, [SIM, name, "--erase"])
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
        sys.exit(f"mkdb: {name}: no {pattern!r} on screen")

    def key(keys, pattern=None, timeout=600):
        os.write(fd, keys.encode())
        time.sleep(0.3)
        return wait(pattern, timeout) if pattern else None

    wait(SCREEN)
    key("F", "ENTER=ACCEPT")
    # The prompt holds a default: delete it, then type the count.
    m = key("\x7f" * 10 + count + "\r", RESULT)
    print(f"{name}: {m.group(0).lower()}")
    key("\r")
    os.write(fd, b"\x03")
    time.sleep(1)


if __name__ == "__main__":
    main()
