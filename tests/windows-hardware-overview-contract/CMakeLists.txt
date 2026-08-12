add_executable(azzs_windows_hardware_overview_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_windows_hardware_overview_contract
  ROLE test
  PRIVATE_LINKS
    azzs::windows_adapter
)

azzs_register_test(
  NAME windows-hardware-overview.contract
  TARGET azzs_windows_hardware_overview_contract
  LABELS contract
)
