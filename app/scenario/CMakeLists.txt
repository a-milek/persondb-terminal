# scenario — everything done *with* the flight store: fill, verify, erase.
# Returns structs, never prints.
target_sources(app PRIVATE flight_scenario.c)
target_include_directories(app PRIVATE .)
