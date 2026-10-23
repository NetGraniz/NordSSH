# REJECTED ENGINE: isolated feasibility experiment, never linked into the product.
include(FetchContent)

# Author's distribution, not the archived Neovim fork. No floating git branches.
FetchContent_Declare(vterm_source
    URL https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz
    URL_HASH SHA256=09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(vterm_source)
add_library(nordssh_vterm STATIC
    ${vterm_source_SOURCE_DIR}/src/encoding.c
    ${vterm_source_SOURCE_DIR}/src/keyboard.c
    ${vterm_source_SOURCE_DIR}/src/mouse.c
    ${vterm_source_SOURCE_DIR}/src/parser.c
    ${vterm_source_SOURCE_DIR}/src/pen.c
    ${vterm_source_SOURCE_DIR}/src/screen.c
    ${vterm_source_SOURCE_DIR}/src/state.c
    ${vterm_source_SOURCE_DIR}/src/unicode.c
    ${vterm_source_SOURCE_DIR}/src/vterm.c)
target_include_directories(nordssh_vterm PUBLIC ${vterm_source_SOURCE_DIR}/include)
if(MSVC)
    target_compile_definitions(nordssh_vterm PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_library(nordssh_terminal STATIC src/terminal/TerminalModel.cpp)
target_include_directories(nordssh_terminal PUBLIC src)
target_link_libraries(nordssh_terminal PUBLIC nordssh_vterm)
if(MSVC)
    target_compile_options(nordssh_terminal PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(nordssh_terminal PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING)
    add_executable(terminal_core_tests tests/TerminalModelTests.cpp)
    target_link_libraries(terminal_core_tests PRIVATE nordssh_terminal)
    if(MSVC)
        target_compile_options(terminal_core_tests PRIVATE /utf-8)
    endif()
    add_test(NAME terminal_core COMMAND terminal_core_tests)
    set_tests_properties(terminal_core PROPERTIES TIMEOUT 30)
endif()
