add_executable(azzs_windows_source_resolution_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_windows_source_resolution_contract
  ROLE test
  PRIVATE_LINKS
    azzs::windows_adapter
)

azzs_register_test(
  NAME windows-source-resolution.contract
  TARGET azzs_windows_source_resolution_contract
  LABELS headless contract
)
