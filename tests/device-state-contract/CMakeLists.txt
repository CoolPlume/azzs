add_executable(azzs_device_state_contract
  main.cpp
)

target_include_directories(azzs_device_state_contract
  PRIVATE
    ../support/include
)

target_link_libraries(azzs_device_state_contract
  PRIVATE
    azzs::application
)

add_test(
  NAME device-state.contract
  COMMAND azzs_device_state_contract
)
