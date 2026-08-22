add_executable(azzs_windows_internet_availability_observer_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_windows_internet_availability_observer_contract
  ROLE test
  PRIVATE_LINKS
    azzs::windows_adapter
)

azzs_register_test(
  NAME windows-internet-availability-observer.contract
  TARGET azzs_windows_internet_availability_observer_contract
  LABELS headless contract
)
