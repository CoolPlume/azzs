add_executable(azzs_windows_emergency_withdrawal_notice_contract
  main.cpp
)

target_compile_definitions(azzs_windows_emergency_withdrawal_notice_contract
  PRIVATE
    AZZS_NOTICE_SOURCE_EXPECTED_ENDPOINT="${AZZS_EMERGENCY_WITHDRAWAL_NOTICE_ENDPOINT}"
)

azzs_project_target(
  TARGET azzs_windows_emergency_withdrawal_notice_contract
  ROLE test
  PRIVATE_LINKS
    azzs::windows_adapter
)

azzs_register_test(
  NAME windows-emergency-withdrawal-notice.contract
  TARGET azzs_windows_emergency_withdrawal_notice_contract
  LABELS headless contract
)
