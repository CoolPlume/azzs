add_executable(azzs_ui_presentation_contract_tests
  presentation_contract_tests.cpp
)

azzs_project_target(
  TARGET azzs_ui_presentation_contract_tests
  ROLE test
  PRIVATE_LINKS
    azzs::ui_presentation_contract
    azzs::software_selection_application
)

azzs_register_test(
  NAME ui.presentation.contract
  TARGET azzs_ui_presentation_contract_tests
  LABELS headless contract
)

add_executable(azzs_settings_navigation_contract_tests
  settings_navigation_contract.cpp
)

azzs_project_target(
  TARGET azzs_settings_navigation_contract_tests
  ROLE test
  PRIVATE_LINKS
    azzs::ui_presentation_contract
)

azzs_register_test(
  NAME ui.settings-navigation.contract
  TARGET azzs_settings_navigation_contract_tests
  LABELS headless contract
)

find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

azzs_register_test(
  NAME ui.design.contract
  LABELS headless contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/check_design_system.py
    ${PROJECT_SOURCE_DIR}
)

azzs_register_test(
  NAME ui.winui.async.contract
  LABELS headless contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/check_winui_async_contract.py
    ${PROJECT_SOURCE_DIR}
)
