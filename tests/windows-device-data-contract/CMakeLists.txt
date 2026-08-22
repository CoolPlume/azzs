add_executable(azzs_windows_device_data_contract
  main.cpp
  windows_device_data_environment_test_seam.cpp
)

azzs_project_target(
  TARGET azzs_windows_device_data_contract
  ROLE test
  PRIVATE_LINKS
    azzs::infrastructure_adapter
    azzs::windows_adapter
    azzs::test_support
)

target_include_directories(azzs_windows_device_data_contract
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

azzs_register_test(
  NAME windows-device-data.contract
  TARGET azzs_windows_device_data_contract
  LABELS headless contract
)

set(azzs_windows_device_data_contract_root
  "${CMAKE_BINARY_DIR}/test-fixtures/windows-device-data")
file(MAKE_DIRECTORY "${azzs_windows_device_data_contract_root}")
set_tests_properties(windows-device-data.contract PROPERTIES
  ENVIRONMENT
    "AZZS_WINDOWS_DEVICE_DATA_CONTRACT_ROOT=${azzs_windows_device_data_contract_root}"
)
