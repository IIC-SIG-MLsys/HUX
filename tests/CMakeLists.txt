# Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
add_executable(hux_contract_tests
  test_main.cpp
  contract/test_lane_failure.cpp
  contract/test_queue_bounds.cpp
  contract/test_control_outbox.cpp
  contract/test_striping.cpp
  contract/test_contract.cpp
  contract/test_dependencies.cpp
  contract/test_control.cpp
  contract/test_fairness.cpp
  contract/test_registration_cache.cpp
  contract/test_failure.cpp
  contract/test_observability.cpp
  contract/test_topology.cpp
  contract/test_local_path.cpp
  contract/test_locality.cpp
  contract/test_stress.cpp
  contract/test_ipc_path.cpp
  contract/test_paths.cpp
  contract/test_mlx5_prm.cpp
  contract/test_progress_turns.cpp
  contract/test_stream_order.cpp
  contract/test_ready_spans.cpp
  device/test_host_backend.cpp
)
target_include_directories(hux_contract_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hux_contract_tests PRIVATE hux_core hux_transport_ipc)
target_compile_options(hux_contract_tests PRIVATE -Wall -Wextra)
add_test(NAME contract COMMAND hux_contract_tests)
# A timeout, because the failures worth catching here are waits. The one
# that prompted this test sat in poll() for 30 s a message; without a bound
# a regression stops looking like a failure and starts looking like CI
# being slow.
set_tests_properties(contract PROPERTIES TIMEOUT 300)

if(TARGET hux_transport_rdma)
  add_executable(hux_rdma_tests
    test_main.cpp
    contract/test_peer_region_ids.cpp
    contract/test_metadata_envelope.cpp
    contract/test_wire_version.cpp
    contract/test_congestion.cpp)
  target_include_directories(hux_rdma_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(hux_rdma_tests PRIVATE hux_transport_rdma)
  target_compile_options(hux_rdma_tests PRIVATE -Wall -Wextra)
  add_test(NAME rdma_wire COMMAND hux_rdma_tests)
  set_tests_properties(rdma_wire PROPERTIES TIMEOUT 300)

  # Two processes and a real NIC, so it is built but never run by ctest.
  # Built here rather than compiled by hand, so it cannot drift from the
  # library it exercises.
  add_executable(hux_rdma_loopback manual/rdma_loopback.cpp)
  target_include_directories(hux_rdma_loopback PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(hux_rdma_loopback PRIVATE hux_transport_rdma)
  target_compile_options(hux_rdma_loopback PRIVATE -Wall -Wextra)
  # Device memory comes from whichever vendor backend is present.
  foreach(vendor cuda rocm neuware)
    string(TOUPPER ${vendor} VENDOR_UP)
    if(TARGET hux_device_${vendor})
      target_compile_definitions(hux_rdma_loopback PRIVATE HUX_LOOPBACK_${VENDOR_UP})
      target_link_libraries(hux_rdma_loopback PRIVATE hux_device_${vendor})
    endif()
  endforeach()

  # The long run: every dimension at once for hours. Two machines and a
  # day of them, so ctest builds it and never runs it.
  add_executable(hux_soak manual/soak.cpp)
  target_include_directories(hux_soak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(hux_soak PRIVATE hux_transport_rdma)
  target_compile_options(hux_soak PRIVATE -Wall -Wextra)
  foreach(vendor cuda rocm neuware)
    string(TOUPPER ${vendor} VENDOR_UP)
    if(TARGET hux_device_${vendor})
      target_compile_definitions(hux_soak PRIVATE HUX_LOOPBACK_${VENDOR_UP})
      target_link_libraries(hux_soak PRIVATE hux_device_${vendor})
    endif()
  endforeach()
endif()

# The UCX provider end to end. Its result depends on which transports UCX
# selects on the host (docs/ucx.md), so ctest builds it and never runs it.
if(TARGET hux_transport_ucx)
  add_executable(hux_ucx_loopback manual/ucx_loopback.cpp)
  target_include_directories(hux_ucx_loopback PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(hux_ucx_loopback PRIVATE hux_transport_ucx)
  target_compile_options(hux_ucx_loopback PRIVATE -Wall -Wextra)
endif()

# Two processes and a device that can export an allocation, so it is built
# but never run by ctest. Built for whichever vendor backend is present.
if(TARGET hux_device_cuda OR TARGET hux_device_rocm OR TARGET hux_device_neuware)
  add_executable(hux_ipc_pair manual/ipc_pair.cpp)
  target_include_directories(hux_ipc_pair PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(hux_ipc_pair PRIVATE hux_transport_ipc)
  target_compile_options(hux_ipc_pair PRIVATE -Wall -Wextra)
  foreach(vendor cuda rocm neuware)
    string(TOUPPER ${vendor} VENDOR_UP)
    if(TARGET hux_device_${vendor})
      target_compile_definitions(hux_ipc_pair PRIVATE HUX_LOOPBACK_${VENDOR_UP})
      target_link_libraries(hux_ipc_pair PRIVATE hux_device_${vendor})
    endif()
  endforeach()
  if(TARGET hux_transport_rdma)
    add_executable(hux_two_paths manual/two_paths.cpp)
    target_include_directories(hux_two_paths PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(hux_two_paths PRIVATE hux_transport_ipc hux_transport_rdma)
    target_compile_options(hux_two_paths PRIVATE -Wall -Wextra)
    foreach(vendor cuda rocm neuware)
      string(TOUPPER ${vendor} VENDOR_UP)
      if(TARGET hux_device_${vendor})
        target_compile_definitions(hux_two_paths PRIVATE HUX_LOOPBACK_${VENDOR_UP})
        target_link_libraries(hux_two_paths PRIVATE hux_device_${vendor})
      endif()
    endforeach()
  endif()
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
