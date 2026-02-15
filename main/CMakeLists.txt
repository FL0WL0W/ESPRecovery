# Generate gzipped HTML at build time
find_program(GZIP_EXECUTABLE gzip REQUIRED)

set(ROOT_HTML_SOURCE "${CMAKE_CURRENT_LIST_DIR}/root.html")
set(ROOT_HTML_GZ "${CMAKE_CURRENT_BINARY_DIR}/root.html.gz")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_custom_command(
        OUTPUT "${ROOT_HTML_GZ}"
        COMMAND ${GZIP_EXECUTABLE} -9 -c "${ROOT_HTML_SOURCE}" > "${ROOT_HTML_GZ}"
        DEPENDS "${ROOT_HTML_SOURCE}"
        COMMENT "Compressing root.html..."
        VERBATIM
    )
endif()

idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_event esp_wifi esp_http_server esp_partition esp_netif lwip freertos app_update nvs_flash dns_server
                       EMBED_FILES "${ROOT_HTML_GZ}")