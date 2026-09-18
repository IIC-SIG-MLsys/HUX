# Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
cmake_minimum_required(VERSION 3.16)
project(hux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# 每个后端独立开关。core 与 mock 不依赖任何 GPU/RDMA SDK，
# 这样无硬件的机器（含 CI）能完整构建并自测。
option(HUX_ENABLE_RDMA    "Native RDMA provider (libibverbs)" OFF)
option(HUX_ENABLE_UCX     "UCX provider"                      OFF)
option(HUX_ENABLE_CUDA    "NVIDIA device backend"             OFF)
option(HUX_ENABLE_ROCM    "AMD / Hygon device backend"        OFF)
option(HUX_ENABLE_NEUWARE "Cambricon device backend"          OFF)
option(HUX_ENABLE_MUSA    "Moore Threads device backend"      OFF)
option(HUX_BUILD_TESTS    "Build tests"                       ON)

# ---- core：纯 C++，无外部依赖 ----
add_library(hux_core
  src/core/strings.cpp
  src/core/region_impl.cpp
  src/core/request_impl.cpp
  src/core/engine_impl.cpp
  src/device/host_backend.cpp
  src/transport/mock/mock_provider.cpp
)
target_include_directories(hux_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
find_package(Threads REQUIRED)
target_link_libraries(hux_core PUBLIC Threads::Threads)
target_compile_options(hux_core PRIVATE -Wall -Wextra)

# ---- 可选后端：启用了就必须找得到，缺依赖在配置阶段失败 ----
# 不允许"找不到就静默跳过"——那会让人以为自己构建出了带 RDMA 的库。
if(HUX_ENABLE_RDMA)
  find_path(IBVERBS_INCLUDE_DIR infiniband/verbs.h)
  find_library(IBVERBS_LIBRARY NAMES ibverbs)
  if(NOT IBVERBS_INCLUDE_DIR OR NOT IBVERBS_LIBRARY)
    message(FATAL_ERROR
      "HUX_ENABLE_RDMA=ON but libibverbs was not found. "
      "Install rdma-core development files, or configure with -DHUX_ENABLE_RDMA=OFF.")
  endif()
  message(STATUS "HUX: RDMA provider enabled (${IBVERBS_LIBRARY})")
endif()

if(HUX_ENABLE_UCX)
  find_path(UCX_INCLUDE_DIR ucp/api/ucp.h)
  find_library(UCX_LIBRARY NAMES ucp)
  if(NOT UCX_INCLUDE_DIR OR NOT UCX_LIBRARY)
    message(FATAL_ERROR
      "HUX_ENABLE_UCX=ON but UCX was not found. "
      "Install libucx-dev, or configure with -DHUX_ENABLE_UCX=OFF.")
  endif()
  message(STATUS "HUX: UCX provider enabled (${UCX_LIBRARY})")
endif()

if(HUX_ENABLE_CUDA)
  find_package(CUDAToolkit QUIET)
  if(NOT CUDAToolkit_FOUND)
    message(FATAL_ERROR "HUX_ENABLE_CUDA=ON but the CUDA toolkit was not found.")
  endif()
  # Compiled as plain C++ against the runtime API; no nvcc needed, so the
  # backend builds wherever the toolkit headers are present.
  add_library(hux_device_cuda src/device/cuda_backend.cpp)
  target_link_libraries(hux_device_cuda PUBLIC hux_core CUDA::cudart)
  target_compile_options(hux_device_cuda PRIVATE -Wall -Wextra)
  message(STATUS "HUX: CUDA device backend enabled (${CUDAToolkit_VERSION})")
endif()

if(HUX_ENABLE_NEUWARE)
  # 寒武纪：注册上限较低且随碎片浮动，设备后端必须如实上报 max_registration_bytes。
  if(NOT DEFINED ENV{NEUWARE_HOME})
    message(FATAL_ERROR "HUX_ENABLE_NEUWARE=ON but NEUWARE_HOME is not set.")
  endif()
  message(STATUS "HUX: Neuware device backend enabled ($ENV{NEUWARE_HOME})")
endif()

if(HUX_ENABLE_ROCM)
  if(NOT DEFINED ENV{ROCM_PATH} AND NOT EXISTS /opt/dtk AND NOT EXISTS /opt/rocm)
    message(FATAL_ERROR "HUX_ENABLE_ROCM=ON but neither ROCM_PATH, /opt/dtk nor /opt/rocm was found.")
  endif()
  message(STATUS "HUX: ROCm/DTK device backend enabled")
endif()

if(HUX_ENABLE_MUSA)
  if(NOT DEFINED ENV{MUSA_HOME} AND NOT EXISTS /usr/local/musa)
    message(FATAL_ERROR "HUX_ENABLE_MUSA=ON but MUSA_HOME was not found.")
  endif()
  message(STATUS "HUX: MUSA device backend enabled")
endif()

if(HUX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "HUX build summary:")
message(STATUS "  RDMA=${HUX_ENABLE_RDMA} UCX=${HUX_ENABLE_UCX}")
message(STATUS "  CUDA=${HUX_ENABLE_CUDA} ROCM=${HUX_ENABLE_ROCM} "
               "NEUWARE=${HUX_ENABLE_NEUWARE} MUSA=${HUX_ENABLE_MUSA}")
