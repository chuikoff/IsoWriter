cmake_minimum_required(VERSION 3.15)
project(IsoWriter VERSION 0.1 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/W3 /utf-8 /wd4996 /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX UNICODE _UNICODE)
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/res
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set(SOURCES
    src/plugin_main.cpp
    src/drive_enum.cpp
    src/iso_detect.cpp
    src/raw_writer.cpp
    src/iso_unpack.cpp
    src/write_dialog.cpp
    src/settings.cpp
    res/resource.rc
    IsoWriter.def
)

add_library(IsoWriter SHARED ${SOURCES})

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(IsoWriter PROPERTIES
        OUTPUT_NAME "IsoWriter"
        SUFFIX ".wfx64"
        PREFIX ""
    )
else()
    set_target_properties(IsoWriter PROPERTIES
        OUTPUT_NAME "IsoWriter"
        SUFFIX ".wfx"
        PREFIX ""
    )
endif()

target_link_libraries(IsoWriter PRIVATE
    kernel32
    user32
    gdi32
    shell32
    advapi32
    virtdisk
)
