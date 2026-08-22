add_executable(azzs_operation_occupancy_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_operation_occupancy_contract
  ROLE test
  PRIVATE_LINKS
    azzs::infrastructure_adapter
    azzs::test_support
)

azzs_register_test(
  NAME operation-occupancy.contract
  TARGET azzs_operation_occupancy_contract
  LABELS headless contract
)
