# flights — the made-up airports and flights, a pure function of an index.
# Depends on flightdb only for the record types it fills in; touches no storage.
target_sources(app PRIVATE flights.c)
target_include_directories(app PRIVATE .)
