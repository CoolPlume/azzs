add_executable(azzs_software_optimization_discovery_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_software_optimization_discovery_contract
  ROLE test
  PRIVATE_LINKS
    azzs::software_optimization_catalog_domain
)

azzs_register_test(
  NAME software-optimization-discovery.contract
  TARGET azzs_software_optimization_discovery_contract
  LABELS headless contract
)
