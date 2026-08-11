add_executable(azzs_ui_presentation_contract
  presentation_contract_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/adapters/ui/winui/DesignSystem/Fixtures/design_system_fixture.cpp
  ${PROJECT_SOURCE_DIR}/src/adapters/ui/winui/DesignSystem/presentation_contract.cpp
)

target_include_directories(azzs_ui_presentation_contract
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/adapters/ui/winui/DesignSystem
)

add_test(
  NAME ui.presentation.contract
  COMMAND azzs_ui_presentation_contract
)

find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

add_test(
  NAME ui.design.contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/check_design_system.py
    ${PROJECT_SOURCE_DIR}
)
