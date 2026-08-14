add_executable(azzs_software_optimization_batch_runner_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_software_optimization_batch_runner_contract
  ROLE test
  PRIVATE_LINKS
    azzs::software_optimization_batch_runner_application
    azzs::software_optimization_batch_runner_domain
    azzs::test_support
)

azzs_register_test(
  NAME software-optimization-batch-runner.contract
  TARGET azzs_software_optimization_batch_runner_contract
  LABELS headless contract
)
