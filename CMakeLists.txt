cmake_minimum_required(VERSION 3.21)

project(StringsX64DbgNext LANGUAGES CXX)

include(CTest)

if(MSVC)
    add_compile_options(/EHsc)
endif()

if(NOT WIN32)
    message(FATAL_ERROR "StringsX64DbgNext is a Windows x64dbg plugin.")
endif()

set(X64DBG_SDK_ROOT "" CACHE PATH "Path to an x64dbg checkout containing pluginsdk/")
if(NOT EXISTS "${X64DBG_SDK_ROOT}/pluginsdk/_plugins.h")
    message(FATAL_ERROR "Set X64DBG_SDK_ROOT to an x64dbg checkout containing pluginsdk/_plugins.h.")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(PLUGIN_ARCH x64)
    set(PLUGIN_BITS 64)
    set(SDK_ARCH_PREFIX x64)
else()
    set(PLUGIN_ARCH x86)
    set(PLUGIN_BITS 32)
    set(SDK_ARCH_PREFIX x32)
endif()

set(PLUGIN_SDK_DIR "${X64DBG_SDK_ROOT}/pluginsdk")
set(DEBUGGER_IMPORT_LIBRARY "${PLUGIN_SDK_DIR}/${SDK_ARCH_PREFIX}dbg.lib")
set(BRIDGE_IMPORT_LIBRARY "${PLUGIN_SDK_DIR}/${SDK_ARCH_PREFIX}bridge.lib")
foreach(import_library IN ITEMS "${DEBUGGER_IMPORT_LIBRARY}" "${BRIDGE_IMPORT_LIBRARY}")
    if(NOT EXISTS "${import_library}")
        message(FATAL_ERROR "Required x64dbg import library not found: ${import_library}")
    endif()
endforeach()

add_library(strings_scanner STATIC
    src/scanner.cpp
    src/scanner.h
)
target_compile_features(strings_scanner PUBLIC cxx_std_17)
target_compile_definitions(strings_scanner PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(strings_scanner PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(strings MODULE
    src/plugin.cpp
)
target_compile_features(strings PRIVATE cxx_std_17)
target_compile_definitions(strings PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0601
)
target_include_directories(strings PRIVATE "${PLUGIN_SDK_DIR}")
target_link_libraries(strings PRIVATE
    strings_scanner
    "${DEBUGGER_IMPORT_LIBRARY}"
    "${BRIDGE_IMPORT_LIBRARY}"
    comctl32
    comdlg32
)
set_target_properties(strings PROPERTIES
    PREFIX ""
    OUTPUT_NAME "strings"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/${PLUGIN_ARCH}/$<CONFIG>"
)

set(PLUGIN_PACKAGE_DIR "${CMAKE_BINARY_DIR}/package/${PLUGIN_ARCH}/$<CONFIG>/plugins")
add_custom_command(TARGET strings POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PLUGIN_PACKAGE_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:strings>"
        "${PLUGIN_PACKAGE_DIR}/strings.dp${PLUGIN_BITS}"
    VERBATIM
)

if(BUILD_TESTING)
    add_executable(scanner-tests tests/scanner_tests.cpp)
    target_link_libraries(scanner-tests PRIVATE strings_scanner)
    add_test(NAME scanner-tests COMMAND scanner-tests)
endif()
