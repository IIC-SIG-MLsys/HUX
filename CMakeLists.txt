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
option(HUX_BUILD_PYTHON  "Python bindings (needs nanobind)"  OFF)

# ---- core：纯 C++，无外部依赖 ----
add_library(hux_core
  src/core/strings.cpp
  src/core/region_impl.cpp
  src/core/request_impl.cpp
  src/core/engine_impl.cpp
  src/control/control_message.cpp
  src/control/identity.cpp
  src/device/host_backend.cpp
  src/device/topology.cpp
  src/transport/control_outbox.cpp
  src/transport/local/local_provider.cpp
  src/transport/mock/mock_provider.cpp
)
target_include_directories(hux_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
find_package(Threads REQUIRED)
target_link_libraries(hux_core PUBLIC Threads::Threads)
target_compile_options(hux_core PRIVATE -Wall -Wextra)

# ---- 同机跨进程路径：只用 POSIX，所以无条件构建 ----
# 能不能用由设备后端回答，不由构建开关回答。
add_library(hux_transport_ipc src/transport/ipc/ipc_provider.cpp)
target_link_libraries(hux_transport_ipc PUBLIC hux_core)
target_compile_options(hux_transport_ipc PRIVATE -Wall -Wextra)

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
  add_library(hux_transport_rdma
    src/transport/rdma/rdma_provider.cpp
    src/transport/cc/controller.cpp)
  target_include_directories(hux_transport_rdma PUBLIC ${IBVERBS_INCLUDE_DIR})
  target_link_libraries(hux_transport_rdma PUBLIC hux_core ${IBVERBS_LIBRARY})
  target_compile_options(hux_transport_rdma PRIVATE -Wall -Wextra)
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
  add_library(hux_transport_ucx src/transport/ucx/ucx_provider.cpp)
  target_include_directories(hux_transport_ucx PUBLIC ${UCX_INCLUDE_DIR})
  target_link_libraries(hux_transport_ucx PUBLIC hux_core ${UCX_LIBRARY})
  target_compile_options(hux_transport_ucx PRIVATE -Wall -Wextra)
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
  # The driver API is needed for one thing only: finding the allocation an
  # address belongs to, which IPC export requires and the runtime API does
  # not expose.
  target_link_libraries(hux_device_cuda PUBLIC hux_core CUDA::cudart CUDA::cuda_driver)
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

if(HUX_BUILD_PYTHON)
  find_package(Python 3.9 COMPONENTS Interpreter Development.Module REQUIRED)
  # nanobind ships its CMake config inside the installed package.
  execute_process(
    COMMAND "${Python_EXECUTABLE}" -c "import nanobind; print(nanobind.cmake_dir())"
    OUTPUT_VARIABLE NB_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE NB_RESULT)
  if(NOT NB_RESULT EQUAL 0)
    message(FATAL_ERROR
      "HUX_BUILD_PYTHON=ON but nanobind was not found. "
      "Install it with: pip install nanobind")
  endif()
  list(APPEND CMAKE_PREFIX_PATH "${NB_DIR}")
  find_package(nanobind CONFIG REQUIRED)

  set(HUX_PY_SOURCES python/hux_py.cpp)
  nanobind_add_module(hux_python NB_STATIC ${HUX_PY_SOURCES})
  set_target_properties(hux_python PROPERTIES OUTPUT_NAME hux)
  target_link_libraries(hux_python PRIVATE hux_core)
  if(TARGET hux_transport_rdma)
    # The RDMA provider is only exposed when it was built; otherwise the
    # module still works over the mock backend.
    target_link_libraries(hux_python PRIVATE hux_transport_rdma)
    target_compile_definitions(hux_python PRIVATE HUX_PY_RDMA)
  endif()
  # A device backend, so the bindings can adapt an application's stream and
  # order a transfer against its events. Without one nothing in Python
  # touches a GPU and the adapters have nothing to adapt.
  foreach(vendor cuda rocm neuware musa)
    string(TOUPPER ${vendor} VENDOR_UP)
    if(TARGET hux_device_${vendor})
      target_compile_definitions(hux_python PRIVATE HUX_PY_${VENDOR_UP})
      target_link_libraries(hux_python PRIVATE hux_device_${vendor})
    endif()
  endforeach()
  if(HUX_BUILD_TESTS)
    enable_testing()
    add_test(NAME python_bindings
             COMMAND "${Python_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/test_bindings.py")
    # The module lands in the build directory, not on the default path.
    set_tests_properties(python_bindings PROPERTIES
                         ENVIRONMENT "PYTHONPATH=${CMAKE_BINARY_DIR}")
  endif()
  message(STATUS "HUX: Python bindings enabled (${NB_DIR})")
endif()

if(HUX_BUILD_TESTS)
  enable_testing()
# Needs two machines, so it is built but never run by ctest. Defined here,
# after every device backend, because it asks whether their targets exist --
# and a target that has not been declared yet simply is not there.
if(TARGET hux_transport_rdma)
  # Needs two machines, so it is built but never run by ctest. Built here
  # rather than compiled by hand, so it cannot drift from the library.
  add_executable(hux-bench benchmarks/hux_bench.cpp)
  target_link_libraries(hux-bench PRIVATE hux_transport_rdma)
  target_compile_options(hux-bench PRIVATE -Wall -Wextra)
  # So it can transfer device memory, which is what a GPU application moves.
  foreach(vendor cuda rocm neuware)
    string(TOUPPER ${vendor} VENDOR_UP)
    if(TARGET hux_device_${vendor})
      target_compile_definitions(hux-bench PRIVATE HUX_BENCH_${VENDOR_UP})
      target_link_libraries(hux-bench PRIVATE hux_device_${vendor})
    endif()
  endforeach()
  if(HUX_BUILD_TESTS)
    enable_testing()
    # The unit tests cannot catch a broken hux-bench, which is a program
    # rather than a target they run. It skips where no adapter is active.
    add_test(NAME loopback_smoke
             COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/loopback_smoke.sh"
                     "$<TARGET_FILE:hux-bench>")
    set_tests_properties(loopback_smoke PROPERTIES TIMEOUT 240)
  endif()
endif()

  add_subdirectory(tests)
endif()

message(STATUS "HUX build summary:")
message(STATUS "  RDMA=${HUX_ENABLE_RDMA} UCX=${HUX_ENABLE_UCX}")
message(STATUS "  CUDA=${HUX_ENABLE_CUDA} ROCM=${HUX_ENABLE_ROCM} "
               "NEUWARE=${HUX_ENABLE_NEUWARE} MUSA=${HUX_ENABLE_MUSA}")

# The UCX baseline for hux-bench. A plain UCP program linked against no part
# of HUX, so what it measures is UCX, not HUX running over UCX. Off by
# default: it needs a UCX install, which most hosts here do not have --
# point CMAKE_PREFIX_PATH at one.
option(HUX_BUILD_UCX_BENCH "UCX baseline for hux-bench (needs UCX)" OFF)
if(HUX_BUILD_UCX_BENCH)
  find_path(UCX_BENCH_INCLUDE_DIR ucp/api/ucp.h)
  find_library(UCX_BENCH_UCP NAMES ucp)
  find_library(UCX_BENCH_UCS NAMES ucs)
  if(NOT UCX_BENCH_INCLUDE_DIR OR NOT UCX_BENCH_UCP OR NOT UCX_BENCH_UCS)
    message(FATAL_ERROR
      "HUX_BUILD_UCX_BENCH=ON but UCX was not found. "
      "Set CMAKE_PREFIX_PATH to a UCX install.")
  endif()
  add_executable(ucx-bench benchmarks/ucx_bench.cpp)
  target_include_directories(ucx-bench PRIVATE ${UCX_BENCH_INCLUDE_DIR})
  target_link_libraries(ucx-bench PRIVATE ${UCX_BENCH_UCP} ${UCX_BENCH_UCS})
  target_compile_options(ucx-bench PRIVATE -Wall -Wextra)
  message(STATUS "HUX: UCX baseline benchmark enabled (${UCX_BENCH_UCP})")
endif()
