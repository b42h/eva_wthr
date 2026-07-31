# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS
    common_components
    )

add_compile_options(-Wno-format)
add_compile_options(-Wno-ignored-qualifiers)
add_compile_options(-Wno-attributes)
add_compile_options(-Wno-unused-function)

# Build version string, exposed as EVA_FW_VERSION. tools/ota.py compares this
# against the value the device reports after a reboot to confirm the NEW image
# is the one running (and not a rollback to the old slot).
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE EVA_GIT_DESC
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT EVA_GIT_DESC)
    set(EVA_GIT_DESC "nogit")
endif()
string(TIMESTAMP EVA_BUILD_STAMP "%Y%m%dT%H%M%S" UTC)
set(EVA_FW_VERSION "${EVA_BUILD_STAMP}-${EVA_GIT_DESC}")
add_compile_definitions(EVA_FW_VERSION="${EVA_FW_VERSION}")
message(STATUS "EVA_FW_VERSION = ${EVA_FW_VERSION}")

project(eva_weather)
