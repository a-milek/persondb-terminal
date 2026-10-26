# ui — the frontends. Exactly one is compiled, chosen by Kconfig; each is
# argument parsing and formatting over scenario.h (and flight_scenario.h).
target_sources(app PRIVATE main.c)
target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_FRONTEND_BENCH app PRIVATE ui_bench.c)
target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_FRONTEND_SHELL app PRIVATE ui_shell.c)
# The full-screen terminal: tui.c draws, and it runs the person (ui_tui.c),
# flight (ui_tui_flights.c) or library (ui_tui_library.c) database: all three
# behind the sign-on screen (ui_tui_main.c), or one alone (ui_tui_single.c).
target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_FRONTEND_TUI app PRIVATE tui.c)
target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_TUI_ALL app PRIVATE ui_tui_main.c)
if(CONFIG_APP_CBOR_PERSONDB_FRONTEND_TUI AND NOT CONFIG_APP_CBOR_PERSONDB_TUI_ALL)
  target_sources(app PRIVATE ui_tui_single.c)
endif()
target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_TUI_HAS_PERSONS app PRIVATE ui_tui.c)
target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_TUI_HAS_FLIGHTS app PRIVATE ui_tui_flights.c)
target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_TUI_HAS_LIBRARY app PRIVATE ui_tui_library.c)
