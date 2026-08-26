add_executable(azzs_hardware_information_presentation_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_hardware_information_presentation_contract
  ROLE test
  PRIVATE_LINKS
    azzs::ui_presentation_contract
)

azzs_register_test(
  NAME hardware-information-presentation.contract
  TARGET azzs_hardware_information_presentation_contract
  LABELS headless contract
)
