
set(www  "${CMAKE_CURRENT_SOURCE_DIR}/../ui/dist")
set(srcs "lightctl.c" "settings.c" "dallas.c" "wifi.c" "http.c")

idf_component_register(SRCS "${srcs}" INCLUDE_DIRS ".")

spiffs_create_partition_image(
	www "${www}" FLASH_IN_PROJECT
	DEPENDS "${www}/index.html.gz"
)

