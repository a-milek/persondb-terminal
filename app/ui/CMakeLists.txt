# ui — the frontends. Exactly one is compiled, chosen by Kconfig; each is
# argument parsing and formatting over scenario.h (and flight_scenario.h).
target_sources(app PRIVATE main.c)
target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_FRONTEND_BENCH app PRIVATE ui_bench.c)
target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_FRONTEND_SHELL app PRIVATE ui_shell.c)
# The full-screen terminal: tui.c draws, ui_tui_main.c is the sign-on screen,
# and it runs the person (ui_tui.c) or flight (ui_tui_flights.c) database.
target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_FRONTEND_TUI app PRIVATE
  tui.c ui_tui_main.c ui_tui.c ui_tui_flights.c)
