add_executable(azzs_installation_batch_creation_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_installation_batch_creation_contract
  ROLE test
  PRIVATE_LINKS
    azzs::installation_batch_runner_application
    azzs::installation_batch_runner_domain
    azzs::test_support
)

azzs_register_test(
  NAME installation-batch-creation.contract
  TARGET azzs_installation_batch_creation_contract
  LABELS headless contract
)
