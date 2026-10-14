# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Argus has board-local components so its AI dependencies stay out of stock builds.
if(BOARD_NAME STREQUAL "argus-k10" OR "$ENV{XIAOZHI_BOARD_NAME}" STREQUAL "argus-k10")
    set(ARGUS_BUILD ON CACHE INTERNAL "Argus K10 board build")
    list(APPEND EXTRA_COMPONENT_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/main/boards/argus/components/argus_face"
        "${CMAKE_CURRENT_SOURCE_DIR}/main/boards/argus/components/argus_voice"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ConversationalAI-Embedded-Kit-2.0/volc_conv_ai"
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.5.2")
project(xiaozhi)
