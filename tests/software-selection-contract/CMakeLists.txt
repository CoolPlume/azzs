add_executable(azzs_software_selection_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_software_selection_contract
  ROLE test
  PRIVATE_LINKS
    azzs::software_selection_application
    azzs::software_selection_domain
    azzs::software_catalog_domain
    azzs::architecture_selection_application
    azzs::architecture_selection_domain
    azzs::application
    azzs::test_support
)

azzs_register_test(
  NAME software-selection.contract
  TARGET azzs_software_selection_contract
  LABELS headless contract
)
