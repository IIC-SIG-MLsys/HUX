# Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
add_executable(hux_contract_tests
  test_main.cpp
  contract/test_contract.cpp
)
target_include_directories(hux_contract_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hux_contract_tests PRIVATE hux_core)
target_compile_options(hux_contract_tests PRIVATE -Wall -Wextra)
add_test(NAME contract COMMAND hux_contract_tests)
