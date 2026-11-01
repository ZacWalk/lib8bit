cmake_minimum_required(VERSION 3.21)

project(lib8bit LANGUAGES CXX VERSION 1.0.0)

# The emulator core is standard C++20 with no platform surface: it builds with
# MSVC on Windows (x64, x86, ARM64) and GCC on Linux. The Win32 test app is
# Windows-only, and neither the app nor the suite is built when another project
# brings lib8bit in through add_subdirectory or FetchContent.
include(CMakeDependentOption)
cmake_dependent_option(LIB8BIT_BUILD_APP "Build the Windows test app" ON "WIN32;PROJECT_IS_TOP_LEVEL" OFF)

if(LIB8BIT_BUILD_APP)
    # Use the sibling checkout while developing, but keep a published commit
    # in the project so a clean configure is reproducible.
    if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_LIBDESKTOP
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../libdesktop/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_LIBDESKTOP "${CMAKE_CURRENT_SOURCE_DIR}/../libdesktop"
            CACHE PATH "Local libdesktop checkout to build against")
    endif()

    include(FetchContent)
    FetchContent_Declare(libdesktop
        GIT_REPOSITORY https://github.com/ZacWalk/libdesktop.git
        GIT_TAG        31aae322af6e0696a9597726705b1121bd31580b
    )
    FetchContent_MakeAvailable(libdesktop)
endif()

function(lib8bit_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /MP /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# Every build tree writes its executables to bin/, so a 32-bit tree carries the
# width in their names, as the Visual Studio projects did, rather than
# overwriting the 64-bit ones.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(lib8bit_bits 32)
else()
    set(lib8bit_bits 64)
endif()

# The emulator core: no platform surface, so the tests can drive it headlessly.
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
lib8bit_warnings(lib8bit)

if(LIB8BIT_BUILD_APP)
    platform_add_app(app8bit
        SOURCES     app/app.cpp
        ICON        app/res/c64.ico
        DESCRIPTION "lib8bit - Commodore 64 emulator"
        OUTPUT_NAME app8bit
    )
    target_link_libraries(app8bit PRIVATE lib8bit::lib8bit)
    lib8bit_warnings(app8bit)
    if(MSVC)
        target_compile_options(app8bit PRIVATE /EHsc)
    endif()

    set_target_properties(app8bit PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PROJECT_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_SOURCE_DIR}/bin"
    )
    # The historical names, app8bit64r / app8bit64d, rather than a plain suffix.
    set_target_properties(app8bit PROPERTIES
        OUTPUT_NAME_DEBUG app8bit${lib8bit_bits}d
        OUTPUT_NAME_RELEASE app8bit${lib8bit_bits}r
    )
endif()

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()
if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
    # The emulator's own console suite, which exercises the core directly.
    add_executable(lib8bit_tests test/emulator-tests.cpp)
    target_link_libraries(lib8bit_tests PRIVATE lib8bit::lib8bit)
    lib8bit_warnings(lib8bit_tests)
    set_target_properties(lib8bit_tests PROPERTIES
        DEBUG_POSTFIX d
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PROJECT_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_SOURCE_DIR}/bin"
    )
    if(lib8bit_bits EQUAL 32)
        set_target_properties(lib8bit_tests PROPERTIES OUTPUT_NAME lib8bit_tests32)
    endif()
    add_test(NAME lib8bit_emulator COMMAND lib8bit_tests)
    set_tests_properties(lib8bit_emulator PROPERTIES TIMEOUT 300 LABELS lib8bit)

    if(LIB8BIT_BUILD_APP)
        add_test(NAME lib8bit_app_manifest COMMAND "${CMAKE_COMMAND}"
            "-DEXE=$<TARGET_FILE:app8bit>" -P "${PROJECT_SOURCE_DIR}/test/manifest.cmake")
        set_tests_properties(lib8bit_app_manifest PROPERTIES TIMEOUT 30 LABELS lib8bit)

        add_test(NAME lib8bit_app_headless COMMAND "$<TARGET_FILE:app8bit>" -test)
        set_tests_properties(lib8bit_app_headless PROPERTIES TIMEOUT 120 LABELS lib8bit)
    endif()
endif()
