add_executable(azzs_windows_driver_rescue_folder_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_windows_driver_rescue_folder_contract
  ROLE test
  PRIVATE_LINKS
    azzs::windows_adapter
    windowsapp
)

azzs_register_test(
  NAME windows-driver-rescue-folder.contract
  TARGET azzs_windows_driver_rescue_folder_contract
  LABELS contract
)
