# ui — the full-screen terminal: tui.c draws, ui_tui_flights.c runs the flight
# database on it and ui_tui_main.c starts it. Parsing and printing only, over
# flight_scenario.h.
target_sources(app PRIVATE main.c tui.c ui_tui_main.c ui_tui_flights.c)
target_include_directories(app PRIVATE .)
