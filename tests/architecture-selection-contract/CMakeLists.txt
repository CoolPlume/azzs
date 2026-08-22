add_executable(azzs_architecture_selection_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_architecture_selection_contract
  ROLE test
  PRIVATE_LINKS
    azzs::architecture_selection_application
    azzs::architecture_selection_domain
    azzs::application
    azzs::test_support
)

azzs_register_test(
  NAME architecture-selection.contract
  TARGET azzs_architecture_selection_contract
  LABELS headless contract
)
