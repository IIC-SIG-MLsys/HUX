# Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
add_executable(hux_contract_tests
  test_main.cpp
  contract/test_contract.cpp
  contract/test_dependencies.cpp
  contract/test_control.cpp
  contract/test_fairness.cpp
  contract/test_registration_cache.cpp
  contract/test_failure.cpp
  contract/test_observability.cpp
  device/test_host_backend.cpp
)
target_include_directories(hux_contract_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hux_contract_tests PRIVATE hux_core)
target_compile_options(hux_contract_tests PRIVATE -Wall -Wextra)
add_test(NAME contract COMMAND hux_contract_tests)

if(TARGET hux_transport_rdma)
  add_executable(hux_rdma_tests
    test_main.cpp
    contract/test_wire_version.cpp
    contract/test_congestion.cpp)
  target_include_directories(hux_rdma_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(hux_rdma_tests PRIVATE hux_transport_rdma)
  target_compile_options(hux_rdma_tests PRIVATE -Wall -Wextra)
  add_test(NAME rdma_wire COMMAND hux_rdma_tests)
endif()

if(HUX_ENABLE_CUDA)
  add_executable(hux_cuda_tests test_main.cpp device/test_cuda_backend.cpp)
  target_include_directories(hux_cuda_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(hux_cuda_tests PRIVATE hux_device_cuda)
  target_compile_options(hux_cuda_tests PRIVATE -Wall -Wextra)
  add_test(NAME cuda_backend COMMAND hux_cuda_tests)
endif()

# One suite per vendor, from the same source: every backend claiming a
# capability must behave identically under it.
foreach(vendor rocm neuware musa)
  string(TOUPPER ${vendor} VENDOR_UP)
  if(TARGET hux_device_${vendor})
    add_executable(hux_${vendor}_tests test_main.cpp device/test_vendor_backend.cpp)
    target_include_directories(hux_${vendor}_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(hux_${vendor}_tests PRIVATE HUX_TEST_${VENDOR_UP})
    target_link_libraries(hux_${vendor}_tests PRIVATE hux_device_${vendor})
    target_compile_options(hux_${vendor}_tests PRIVATE -Wall -Wextra)
    add_test(NAME ${vendor}_backend COMMAND hux_${vendor}_tests)
  endif()
endforeach()
