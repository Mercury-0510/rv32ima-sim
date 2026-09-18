cmake_minimum_required(VERSION 3.16)

project(rv32ima-sim LANGUAGES C)

# 为编辑器提供编译参数。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

function(configure_sim_target target)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

add_library(sim_memory STATIC
    src/riscvsim/system/memory.c
    src/riscvsim/system/loader.c
)
configure_sim_target(sim_memory)

add_library(rv32_core STATIC
    src/riscvsim/core/core.c
    src/riscvsim/core/fetch.c
    src/riscvsim/core/memory.c
    src/riscvsim/core/commit.c
    src/riscvsim/core/atomic.c
    src/riscvsim/core/decode.c
    src/riscvsim/core/execute.c
    src/riscvsim/core/csr.c
    src/riscvsim/utils/trace.c
)
target_include_directories(rv32_core PUBLIC src/riscvsim)
configure_sim_target(rv32_core)

add_executable(sim src/riscvsim/main.c)
target_link_libraries(sim PRIVATE rv32_core sim_memory)
configure_sim_target(sim)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    foreach(test rv32i rv32a csr)
        add_executable(test_${test} tests/test_${test}.c)
        target_link_libraries(test_${test} PRIVATE rv32_core)
        configure_sim_target(test_${test})
    endforeach()

    add_executable(test_memory tests/test_memory.c)
    target_link_libraries(test_memory PRIVATE sim_memory)
    target_include_directories(test_memory PRIVATE src/riscvsim)
    configure_sim_target(test_memory)

    add_test(NAME regression
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.py
                --sim $<TARGET_FILE:sim>
                --rv32i $<TARGET_FILE:test_rv32i>
                --rv32a $<TARGET_FILE:test_rv32a>
                --csr $<TARGET_FILE:test_csr>
                --memory $<TARGET_FILE:test_memory>)
endif()

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
    file(GLOB_RECURSE FORMAT_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/riscvsim/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/riscvsim/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h")
    add_custom_target(fmt
        COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${FORMAT_SOURCES}
        COMMENT "Formatting project C sources"
        VERBATIM)
endif()
