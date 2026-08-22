add_executable(azzs_startup_assembly_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_startup_assembly_contract
  ROLE test
  PRIVATE_LINKS
    azzs::test_support
)

azzs_register_test(
  NAME startup-assembly.contract
  TARGET azzs_startup_assembly_contract
  LABELS headless contract
)
