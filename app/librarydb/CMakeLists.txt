# librarydb — the library catalogue API, and the only layer of the library
# database that talks to the storage stack. library_cbor is private to it.
target_sources(app PRIVATE librarydb.c library_cbor.c)
target_include_directories(app PRIVATE .)
