cmake_minimum_required(VERSION 3.16)
project(kernelsched LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ------------------------------- toolchain ---------------------------------
find_program(CLANG_EXECUTABLE clang REQUIRED)
find_program(BPFTOOL_EXECUTABLE bpftool REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf)

execute_process(COMMAND uname -m OUTPUT_VARIABLE KS_ARCH
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(KS_ARCH MATCHES "x86_64")
  set(KS_BPF_ARCH x86)
elseif(KS_ARCH MATCHES "aarch64")
  set(KS_BPF_ARCH arm64)
else()
  message(FATAL_ERROR "Unsupported architecture: ${KS_ARCH}")
endif()
message(STATUS "KernelSched: arch ${KS_ARCH} (BPF arch ${KS_BPF_ARCH})")

# ------------------------------ eBPF pipeline ------------------------------
# 1. vmlinux.h  : generated from the running kernel's BTF (CO-RE).
# 2. .bpf.o     : clang -target bpf
# 3. .skel.h    : bpftool gen skeleton
set(BPF_DIR   ${CMAKE_SOURCE_DIR}/bpf)
set(BPF_BUILD ${CMAKE_BINARY_DIR}/bpf)
file(MAKE_DIRECTORY ${BPF_BUILD})

set(VMLINUX_H ${BPF_BUILD}/vmlinux.h)
add_custom_command(
  OUTPUT ${VMLINUX_H}
  COMMAND /bin/sh -c "${BPFTOOL_EXECUTABLE} btf dump file /sys/kernel/btf/vmlinux format c > ${VMLINUX_H}"
  DEPENDS /sys/kernel/btf/vmlinux
  VERBATIM)

set(BPF_OBJ ${BPF_BUILD}/kernelsched.bpf.o)
add_custom_command(
  OUTPUT ${BPF_OBJ}
  COMMAND ${CLANG_EXECUTABLE} -target bpf -D__TARGET_ARCH_${KS_BPF_ARCH}
          -O2 -g -Wall -Wno-unused-function
          -I${BPF_DIR} -I${BPF_BUILD}
          -c ${BPF_DIR}/kernelsched.bpf.c -o ${BPF_OBJ}
  DEPENDS ${BPF_DIR}/kernelsched.bpf.c ${BPF_DIR}/kernelsched.bpf.h ${VMLINUX_H}
  VERBATIM)

set(BPF_SKEL ${BPF_BUILD}/kernelsched.skel.h)
add_custom_command(
  OUTPUT ${BPF_SKEL}
  COMMAND /bin/sh -c "${BPFTOOL_EXECUTABLE} gen skeleton ${BPF_OBJ} > ${BPF_SKEL}"
  DEPENDS ${BPF_OBJ}
  VERBATIM)

add_custom_target(kernelsched_bpf ALL DEPENDS ${BPF_OBJ} ${BPF_SKEL})

# ------------------------------ userspace CLI ------------------------------
add_executable(kernelsched
  src/main.cpp
  src/tracer.cpp
  src/metrics.cpp
  src/workload.cpp)

add_dependencies(kernelsched kernelsched_bpf)
target_include_directories(kernelsched PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${BPF_BUILD})
target_compile_definitions(kernelsched PRIVATE _GNU_SOURCE)
target_link_libraries(kernelsched PRIVATE PkgConfig::LIBBPF pthread)
target_compile_options(kernelsched PRIVATE -Wall -Wextra)
