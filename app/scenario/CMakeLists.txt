# scenario — everything done *with* a population. Returns structs, never
# prints, so both frontends measure the same thing. The flight database has
# its own, smaller set of operations, used only by the full-screen terminal.
target_sources(app PRIVATE scenario.c)
target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_FRONTEND_TUI app PRIVATE flight_scenario.c
  library_scenario.c)
target_include_directories(app PRIVATE .)
