add_executable(azzs_windows_device_data_contract
  main.cpp
)

target_include_directories(azzs_windows_device_data_contract
  PRIVATE
    ../support/include
)

target_link_libraries(azzs_windows_device_data_contract
  PRIVATE
    azzs::infrastructure_adapter
    azzs::windows_adapter
)

add_test(
  NAME windows-device-data.contract
  COMMAND azzs_windows_device_data_contract
)
