# 3.20 是下限而不是 3.16：文档与 CI 都用 ctest --test-dir，该选项 3.20 才引入。
cmake_minimum_required(VERSION 3.20)

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

# 轨迹功能（阶段日志与提交轨迹）的编译期总开关。关闭时两个 --trace 选项仍然
# 能解析，但会以退出码 2 拒绝；依赖轨迹的测试在运行期自报跳过。
option(ENABLE_TRACE "Compile the stage and commit trace support" ON)

add_library(rv32_core STATIC
    src/riscvsim/core/core.c
    src/riscvsim/core/fetch.c
    src/riscvsim/core/memory.c
    src/riscvsim/core/commit.c
    src/riscvsim/core/atomic.c
    src/riscvsim/core/decode.c
    src/riscvsim/core/execute.c
    src/riscvsim/core/csr.c
)
if(ENABLE_TRACE)
    target_sources(rv32_core PRIVATE src/riscvsim/utils/trace.c)
endif()
target_include_directories(rv32_core PUBLIC src/riscvsim)
# 必须用 $<BOOL:> 把 ON/OFF 转成 1/0：直接写 ${ENABLE_TRACE} 会得到
# -DENABLE_TRACE=ON，而 #if 里的 ON 不是宏、求值为 0，会把功能静默关掉。
target_compile_definitions(rv32_core PUBLIC ENABLE_TRACE=$<BOOL:${ENABLE_TRACE}>)
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

    add_executable(test_commit tests/test_commit.c)
    target_link_libraries(test_commit PRIVATE rv32_core sim_memory)
    target_include_directories(test_commit PRIVATE src/riscvsim)
    configure_sim_target(test_commit)

    add_executable(test_loader tests/test_loader.c)
    target_link_libraries(test_loader PRIVATE sim_memory)
    target_include_directories(test_loader PRIVATE src/riscvsim)
    configure_sim_target(test_loader)

    # test_cli 只调用系统接口：它测的是 sim 的进程契约，不包含项目头。
    add_executable(test_cli tests/test_cli.c)
    add_dependencies(test_cli sim)
    # 不链项目库就拿不到 rv32_core 的 PUBLIC 定义，单独传一份。
    target_compile_definitions(test_cli PRIVATE ENABLE_TRACE=$<BOOL:${ENABLE_TRACE}>)
    configure_sim_target(test_cli)

    # 每个测试二进制自带断言与用例计数，CTest 只看退出码。
    foreach(test rv32i rv32a csr memory commit loader)
        add_test(NAME ${test} COMMAND test_${test})
    endforeach()
    add_test(NAME cli COMMAND test_cli $<TARGET_FILE:sim>
                            ${CMAKE_CURRENT_SOURCE_DIR}/programs/diff_smoke.elf)
    add_test(NAME tools_trace COMMAND ${Python3_EXECUTABLE}
                                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_trace_tools.py)
    add_test(NAME tools_spike COMMAND ${Python3_EXECUTABLE}
                                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spike_tools.py)
    # 起子进程的用例万一挂住，要在 60 秒内失败，而不是拖到 CTest 的默认超时。
    set_tests_properties(rv32i rv32a csr memory commit loader cli tools_trace tools_spike
        PROPERTIES TIMEOUT 60)
    # ENABLE_TRACE=OFF 时 test_commit 只打印跳过行，用 SKIP_RETURN_CODE 如实报成
    # 「跳过」，而不是一条什么都没断言的绿色通过。
    set_tests_properties(commit PROPERTIES SKIP_RETURN_CODE 77)
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
