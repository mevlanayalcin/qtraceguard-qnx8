cmake_minimum_required(VERSION 3.16)
project(QTraceGuard VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(QNXNTO)
    add_compile_definitions(_QNX_SOURCE)
    set(QTG_THREAD_LIB "")
else()
    find_package(Threads REQUIRED)
    set(QTG_THREAD_LIB Threads::Threads)
endif()

if(NOT QNXNTO)
    add_library(qtg_core STATIC
        src/host/qtg_kev.c
        src/host/qtg_analyzer.c
        src/target/qtg_rtos_shim.c
        src/target/qtg_resmgr_node.c
    )
    target_include_directories(qtg_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(qtg_core PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(qtg_core PUBLIC ${QTG_THREAD_LIB} m)

    add_executable(qtraceguard src/host/main.c)
    target_link_libraries(qtraceguard PRIVATE qtg_core)
endif()

add_executable(qtg-resmgr-node
    src/host/qtg_kev.c
    src/target/qtg_rtos_shim.c
    src/target/qtg_resmgr_node.c
)
target_include_directories(qtg-resmgr-node PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(qtg-resmgr-node PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(qtg-resmgr-node PRIVATE QTG_BUILD_RESMGR_BIN)
target_link_libraries(qtg-resmgr-node PRIVATE ${QTG_THREAD_LIB})

add_executable(qtg-probe
    src/host/qtg_kev.c
    src/target/qtg_rtos_shim.c
    src/target/qtg_probe.c
)
target_include_directories(qtg-probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(qtg-probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(qtg-probe PRIVATE ${QTG_THREAD_LIB})

enable_testing()
if(NOT QNXNTO)
    add_executable(test_runner tests/test_runner.c)
    target_link_libraries(test_runner PRIVATE qtg_core)
    add_test(NAME qtg_unit_integration COMMAND test_runner)
endif()
