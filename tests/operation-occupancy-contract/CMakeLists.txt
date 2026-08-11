add_executable(azzs_operation_occupancy_contract
  main.cpp
)

target_include_directories(azzs_operation_occupancy_contract
  PRIVATE
    ../support/include
)

target_link_libraries(azzs_operation_occupancy_contract
  PRIVATE
    azzs::infrastructure_adapter
)

add_test(
  NAME operation-occupancy.contract
  COMMAND azzs_operation_occupancy_contract
)
