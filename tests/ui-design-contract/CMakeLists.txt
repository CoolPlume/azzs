add_executable(azzs_ui_presentation_contract_tests
  presentation_contract_tests.cpp
)

azzs_project_target(
  TARGET azzs_ui_presentation_contract_tests
  ROLE test
  PRIVATE_LINKS
    azzs::ui_presentation_contract
)

azzs_register_test(
  NAME ui.presentation.contract
  TARGET azzs_ui_presentation_contract_tests
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
