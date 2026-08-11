add_executable(azzs_software_optimization_catalog_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_software_optimization_catalog_contract
  ROLE test
  PRIVATE_LINKS
    azzs::software_optimization_catalog_file_adapter
    azzs::infrastructure_adapter
    azzs::test_support
)

azzs_register_test(
  NAME software-optimization-catalog.contract
  TARGET azzs_software_optimization_catalog_contract
  LABELS headless contract
)
