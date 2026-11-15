idf_component_register(
    SRCS "main.c"
         "reader_app.c"
         "reader_ui.c"
         "reader_text.c"
         "reader_layout.c"
         "reader_storage.c"
         "reader_progress.c"
         "reader_bookmark.c"
         "reader_store.c"
         "reader_transfer.c"
         "reader_fonts.c"
         "builtin_books.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "${CMAKE_CURRENT_LIST_DIR}/../assets/web/transfer.html"
    REQUIRES bsp nvs_flash esp_wifi esp_netif esp_event esp_http_server
             spiffs esp_timer lwip
)

# Generated Chinese bitmap fonts live in assets/ and are compiled into flash.
# One file per selectable body size (12..20 px); reader_layout.h owns the list.
set(READER_FONT_SIZES 12 13 14 15 16 17 18 19 20)
foreach(size ${READER_FONT_SIZES})
    target_sources(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/lv_font_reader_${size}.c")
endforeach()
