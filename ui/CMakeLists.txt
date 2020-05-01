
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/dist/index.html.gz
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/yarn.lock
	COMMAND rm -rf dist && yarn build && gzip -9 dist/*
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

set_directory_properties(
	PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES ${CMAKE_CURRENT_SOURCE_DIR}/dist
)

add_custom_target(lightctl_ui DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dist/index.html.gz)

