cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_CURRENT_LIST_DIR}/main/version.h" _hp10_version_h)
string(REGEX MATCH "#define[ \t]+HP10_VERSION[ \t]+\"([^\"]+)\"" _hp10_version_match "${_hp10_version_h}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "Set HP10_VERSION in main/version.h")
endif()
set(PROJECT_VER "${CMAKE_MATCH_1}")
if(NOT PROJECT_VER MATCHES "^[A-Za-z0-9._-]+$")
    message(FATAL_ERROR "HP10_VERSION is the .bin file name. Use letters, digits, dot, underscore, or hyphen.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/main/version.h")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hp10_bringup)

# esptool names the image after the project. Publish the same bytes as <HP10_VERSION>.bin.
if(TARGET gen_project_binary)
    set(_hp10_app_bin "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin")
    set(_hp10_named_bin "${CMAKE_BINARY_DIR}/${PROJECT_VER}.bin")
    add_custom_target(hp10_named_bin ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_hp10_app_bin}" "${_hp10_named_bin}"
        COMMAND ${CMAKE_COMMAND} -E echo "Application image ${_hp10_named_bin}"
        VERBATIM
    )
    add_dependencies(hp10_named_bin gen_project_binary)
endif()
