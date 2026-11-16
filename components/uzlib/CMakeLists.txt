# Minimal vendored inflate-only build of uzlib (zlib license).
# Only the IDF firmware needs the C sources; host tests compile them directly.
idf_component_register(
    SRCS "tinflate.c" "crc32.c" "adler32.c"
    INCLUDE_DIRS "."
)
