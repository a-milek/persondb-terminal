# flightdb — the flight information API, and the only layer of the flight
# database that talks to the storage stack. flight_cbor is private to it.
target_sources(app PRIVATE flightdb.c flight_cbor.c)
target_include_directories(app PRIVATE .)
