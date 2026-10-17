cmake_minimum_required(VERSION 3.16)
project(biobot_firmware)

set(CMAKE_CXX_STANDARD 17)

# ---------------------------------------------------------------------------
# CLion <-> PlatformIO glue.
#
# CMake is NOT the build system here - PlatformIO is (see platformio.ini).
# This file only gives CLion a project to open and index.
#
# `pio project init --ide clion` (or opening the folder with the PlatformIO
# plugin installed) generates CMakeListsPrivate.txt, which carries the
# toolchain paths, framework include dirs and -D defines for the selected
# environment. That file is machine-specific, so it is gitignored and this
# file degrades gracefully when it is missing.
# ---------------------------------------------------------------------------
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/CMakeListsPrivate.txt)
    include(CMakeListsPrivate.txt)

    add_custom_target(
            Production ALL
            COMMAND platformio -c clion run "$<$<NOT:$<CONFIG:All>>:-e${CMAKE_BUILD_TYPE}>"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_custom_target(
            Debug ALL
            COMMAND platformio -c clion run --target debug "$<$<NOT:$<CONFIG:All>>:-e${CMAKE_BUILD_TYPE}>"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
else ()
    message(WARNING
            "CMakeListsPrivate.txt not found - indexing sources only, with no "
            "Arduino/ESP32 headers. Run `pio project init --ide clion` in this "
            "directory (or let the CLion PlatformIO plugin do it) to get full "
            "code insight and build targets.")

    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
    file(GLOB_RECURSE SRC_LIST
            ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
endif ()

# Hand-written overrides that survive a PlatformIO regeneration.
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/CMakeListsUser.txt)
    include(CMakeListsUser.txt)
endif ()

# Never built or flashed - it exists so CLion resolves symbols across files.
add_executable(Z_DUMMY_TARGET ${SRC_LIST})
