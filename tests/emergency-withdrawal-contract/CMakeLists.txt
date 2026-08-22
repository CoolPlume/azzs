add_executable(azzs_emergency_withdrawal_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_emergency_withdrawal_contract
  ROLE test
  PRIVATE_LINKS
    azzs::application
    azzs::test_support
)

azzs_register_test(
  NAME emergency-withdrawal.contract
  TARGET azzs_emergency_withdrawal_contract
  LABELS headless contract
)
