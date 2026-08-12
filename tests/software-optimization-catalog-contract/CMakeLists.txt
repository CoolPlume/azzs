add_executable(azzs_software_optimization_catalog_contract
  main.cpp
)

target_compile_definitions(azzs_software_optimization_catalog_contract
  PRIVATE
    AZZS_SOFTWARE_OPTIMIZATION_CATALOG_PATH="${PROJECT_SOURCE_DIR}/catalog/software-optimization-catalog.toml"
)

azzs_project_target(
  TARGET azzs_software_optimization_catalog_contract
  ROLE test
  PRIVATE_LINKS
    azzs::software_optimization_catalog_file_adapter
    azzs::software_optimization_catalog_application
    azzs::software_optimization_catalog_domain
    azzs::software_catalog_domain
    azzs::infrastructure_adapter
    azzs::test_support
)

azzs_register_test(
  NAME software-optimization-catalog.contract
  TARGET azzs_software_optimization_catalog_contract
  LABELS headless contract
)
