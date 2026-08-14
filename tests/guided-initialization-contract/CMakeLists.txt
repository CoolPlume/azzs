add_executable(azzs_guided_initialization_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_guided_initialization_contract
  ROLE test
  PRIVATE_LINKS
    azzs::guided_initialization_application
    azzs::test_support
)

azzs_register_test(
  NAME guided-initialization.contract
  TARGET azzs_guided_initialization_contract
  LABELS headless contract
)
