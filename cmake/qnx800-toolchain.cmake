# Clean-Room QNX SDP 8.0 Cross-Compilation CMake Toolchain File
# SPDX-License-Identifier: MIT
# Usage:
#   source ~/qnx800/qnxsdp-env.sh
#   cmake -B build-qnx -DCMAKE_TOOLCHAIN_FILE=cmake/qnx800-toolchain.cmake -DQNX_ARCH=x86_64
#   cmake --build build-qnx

set(CMAKE_SYSTEM_NAME QNX)
set(QNXNTO TRUE)

if(NOT DEFINED ENV{QNX_HOST} OR NOT DEFINED ENV{QNX_TARGET})
    message(FATAL_ERROR "QNX_HOST / QNX_TARGET not set. Run 'source ~/qnx800/qnxsdp-env.sh' first.")
endif()

if(NOT DEFINED QNX_ARCH)
    set(QNX_ARCH "x86_64" CACHE STRING "QNX target architecture (x86_64 or aarch64le)")
endif()

set(CMAKE_SYSTEM_PROCESSOR ${QNX_ARCH})
set(CMAKE_C_COMPILER "$ENV{QNX_HOST}/usr/bin/qcc")
set(CMAKE_C_COMPILER_TARGET "gcc_nto${QNX_ARCH}")
set(CMAKE_SYSROOT "$ENV{QNX_TARGET}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
