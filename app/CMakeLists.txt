#-------------------------------------------------------------------------------
# flight-terminal — a made-up airline's flight database, on a full-screen
# serial terminal, over the L2 Map shape
#
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(flight_terminal LANGUAGES C)

# One directory per layer, added bottom-up. Each subdirectory contributes its
# own sources and publishes its own header directory, so the build file has
# the same shape as the design — and a new dependency between layers is a
# visible edit here, not an #include someone slipped in.
#
#   ui/        the terminal: parse and print only
#     |
#   scenario/  operations on the store: fill, verify, erase
#     |   \_______________________
#     |                           |
#   flightdb/                  flights/
#     the flight information      the made-up airports and flights:
#     API, and the only layer     a pure function of an index. Uses
#     that touches storage        flightdb's record types; touches
#                                 no storage.
#     \__ flight_cbor: the wire format, private to flightdb
#
add_subdirectory(flights)
add_subdirectory(flightdb)
add_subdirectory(scenario)
add_subdirectory(ui)
