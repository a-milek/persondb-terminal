# library — the made-up books and loans, a pure function of an index.
# Depends on librarydb only for the record types it fills in; touches no storage.
target_sources(app PRIVATE library.c)
target_include_directories(app PRIVATE .)
