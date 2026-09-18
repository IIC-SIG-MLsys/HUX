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
  if(NOT NEUWARE_HOME)
    if(DEFINED ENV{NEUWARE_HOME})
      set(NEUWARE_HOME $ENV{NEUWARE_HOME})
    else()
      set(NEUWARE_HOME /usr/local/neuware)
    endif()
  endif()
  if(NOT EXISTS ${NEUWARE_HOME}/include/cnrt.h)
    message(FATAL_ERROR
      "HUX_ENABLE_NEUWARE=ON but cnrt.h was not found under ${NEUWARE_HOME}. "
      "Set -DNEUWARE_HOME=<path>, or configure with -DHUX_ENABLE_NEUWARE=OFF.")
  endif()
  add_library(hux_device_neuware src/device/neuware_backend.cpp)
  target_include_directories(hux_device_neuware PUBLIC ${NEUWARE_HOME}/include)
  target_link_directories(hux_device_neuware PUBLIC ${NEUWARE_HOME}/lib64)
  target_link_libraries(hux_device_neuware PUBLIC hux_core cnrt cndrv)
  target_compile_options(hux_device_neuware PRIVATE -Wall -Wextra)
  message(STATUS "HUX: Neuware device backend enabled (${NEUWARE_HOME})")
endif()

if(HUX_ENABLE_ROCM)
  if(NOT ROCM_PATH)
    if(DEFINED ENV{ROCM_PATH})
      set(ROCM_PATH $ENV{ROCM_PATH})
    elseif(EXISTS /opt/dtk)
      set(ROCM_PATH /opt/dtk)          # Hygon DTK
    else()
      set(ROCM_PATH /opt/rocm)
    endif()
  endif()
  if(NOT EXISTS ${ROCM_PATH}/include/hip/hip_runtime.h)
    message(FATAL_ERROR
      "HUX_ENABLE_ROCM=ON but hip_runtime.h was not found under ${ROCM_PATH}. "
      "Set -DROCM_PATH=<path>, or configure with -DHUX_ENABLE_ROCM=OFF.")
  endif()
  add_library(hux_device_rocm src/device/rocm_backend.cpp)
  # The HIP headers refuse to compile unless the platform is pinned.
  target_compile_definitions(hux_device_rocm PUBLIC __HIP_PLATFORM_AMD__)
  target_include_directories(hux_device_rocm PUBLIC ${ROCM_PATH}/include)
  target_link_directories(hux_device_rocm PUBLIC ${ROCM_PATH}/lib)
  target_link_libraries(hux_device_rocm PUBLIC hux_core amdhip64)
  target_compile_options(hux_device_rocm PRIVATE -Wall -Wextra)
  message(STATUS "HUX: ROCm/DTK device backend enabled (${ROCM_PATH})")
endif()

if(HUX_ENABLE_MUSA)
  if(NOT MUSA_HOME)
    if(DEFINED ENV{MUSA_HOME})
      set(MUSA_HOME $ENV{MUSA_HOME})
    else()
      set(MUSA_HOME /usr/local/musa)
    endif()
  endif()
  if(NOT EXISTS ${MUSA_HOME}/include/musa_runtime.h)
    message(FATAL_ERROR
      "HUX_ENABLE_MUSA=ON but musa_runtime.h was not found under ${MUSA_HOME}. "
      "Set -DMUSA_HOME=<path>, or configure with -DHUX_ENABLE_MUSA=OFF.")
  endif()
  # libmusart depends on libmusa, which some installations ship in a separate
  # toolkit directory; MUSA_LIB_DIR points at whichever holds both.
  if(NOT MUSA_LIB_DIR)
    set(MUSA_LIB_DIR ${MUSA_HOME}/lib)
  endif()
  add_library(hux_device_musa src/device/musa_backend.cpp)
  target_include_directories(hux_device_musa PUBLIC ${MUSA_HOME}/include)
  target_link_directories(hux_device_musa PUBLIC ${MUSA_LIB_DIR} ${MUSA_HOME}/lib)
  target_link_libraries(hux_device_musa PUBLIC hux_core musart musa)
  target_compile_options(hux_device_musa PRIVATE -Wall -Wextra)
  message(STATUS "HUX: MUSA device backend enabled (${MUSA_HOME}, libs ${MUSA_LIB_DIR})")
endif()

if(HUX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "HUX build summary:")
message(STATUS "  RDMA=${HUX_ENABLE_RDMA} UCX=${HUX_ENABLE_UCX}")
message(STATUS "  CUDA=${HUX_ENABLE_CUDA} ROCM=${HUX_ENABLE_ROCM} "
               "NEUWARE=${HUX_ENABLE_NEUWARE} MUSA=${HUX_ENABLE_MUSA}")
