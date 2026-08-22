add_executable(azzs_windows_software_presence_detector_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_windows_software_presence_detector_contract
  ROLE test
  PRIVATE_LINKS
    azzs::windows_adapter
)

azzs_register_test(
  NAME windows-software-presence-detector.contract
  TARGET azzs_windows_software_presence_detector_contract
  LABELS headless contract
)
