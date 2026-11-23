idf_component_register(
    SRCS "main.c"
         "reader_app.c"
         "reader_ui.c"
         "reader_text.c"
         "reader_layout.c"
         "reader_pagemap.c"
         "reader_settings.c"
         "reader_storage.c"
         "reader_progress.c"
         "reader_bookmark.c"
         "reader_store.c"
         "reader_zdx.c"
         "reader_transfer.c"
         "reader_fonts.c"
         "reader_fallback.c"
         "builtin_books.c"
         "builtin_books_data.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "${CMAKE_CURRENT_LIST_DIR}/../assets/web/transfer.html"
    REQUIRES bsp nvs_flash esp_wifi esp_netif esp_event esp_http_server
             littlefs esp_partition esp_timer lwip uzlib
)

# Leave room in the HTTP task's 6 KiB stack for request dispatch and storage I/O.
set_source_files_properties(reader_transfer.c PROPERTIES
    COMPILE_OPTIONS "-Werror=frame-larger-than=2048")

# Generated Chinese bitmap fonts live in assets/ and are compiled into flash.
# One file per selectable body size; reader_layout.h owns the list. 17 px and
# 19 px are intentionally absent (upscaled duplicates of 18/20 px) so their
# Flash goes to the book store instead.
set(READER_FONT_SIZES 12 13 14 15 16 18 20)
foreach(size ${READER_FONT_SIZES})
    target_sources(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/lv_font_reader_${size}.c")
endforeach()
