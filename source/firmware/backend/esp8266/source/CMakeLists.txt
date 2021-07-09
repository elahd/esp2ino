# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp2ino)

# PROPERTY OBJECT_DEPENDS.... ensures that updates to the frontend binary are not ignored on build.
# CMake tends to ignore changes to header files.
idf_build_set_property(COMPILE_OPTIONS "-Wno-deprecated-declarations" "-Wno-dev" APPEND PROPERTY OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/static/index.html.gz.h)