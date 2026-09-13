cmake_minimum_required(VERSION 3.21)

project(lib8bit LANGUAGES CXX VERSION 1.0.0)

if(NOT WIN32)
    message(FATAL_ERROR "lib8bit's application layer is Win32-only; build it on Windows.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "lib8bit requires MSVC; no other toolchain is supported.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "lib8bit is 64-bit only; configure with an x64 toolchain.")
endif()

enable_language(RC)

# The emulator core: no Win32 surface, so the tests can drive it headlessly.
add_library(lib8bit STATIC
    src/assembler.cpp
    src/cartridge.cpp
    src/cpu.cpp
    src/debug.cpp
    src/disk.cpp
    src/machine.cpp
    src/opcodes.cpp
    src/rom-chars.cpp
    src/rom-kernal-basic.cpp
    src/rom-kernal.cpp
    src/sid.cpp
    src/vic.cpp
)
add_library(lib8bit::lib8bit ALIAS lib8bit)

target_compile_features(lib8bit PUBLIC cxx_std_20)
target_include_directories(lib8bit PUBLIC src)
target_compile_definitions(lib8bit PUBLIC WIN32 _WINDOWS UNICODE _UNICODE)
target_compile_options(lib8bit PRIVATE /W3 /MP /permissive-)

add_executable(app8bit WIN32
    app/app.cpp
    app/app.rc
)
target_link_libraries(app8bit PRIVATE lib8bit::lib8bit)
target_include_directories(app8bit PRIVATE app)
target_compile_options(app8bit PRIVATE /W3 /MP /permissive-)

set_target_properties(app8bit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
)
# The historical names are app8bit64r / app8bit64d rather than a plain suffix.
set_target_properties(app8bit PROPERTIES OUTPUT_NAME_DEBUG app8bit64d OUTPUT_NAME_RELEASE app8bit64r)

include(CTest)
if(BUILD_TESTING)
    # The emulator's own console suite, which exercises the core directly.
    add_executable(lib8bit_tests test/emulator-tests.cpp)
    target_link_libraries(lib8bit_tests PRIVATE lib8bit::lib8bit)
    target_compile_options(lib8bit_tests PRIVATE /W3 /MP /permissive-)
    set_target_properties(lib8bit_tests PROPERTIES
        DEBUG_POSTFIX d
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
    )
    add_test(NAME lib8bit_emulator COMMAND lib8bit_tests)
    set_tests_properties(lib8bit_emulator PROPERTIES TIMEOUT 300 LABELS lib8bit)
endif()
