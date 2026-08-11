add_library(azzs_ui_presentation_contract STATIC
  Fixtures/design_system_fixture.cpp
  presentation_contract.cpp
)

add_library(azzs::ui_presentation_contract ALIAS
  azzs_ui_presentation_contract)

target_include_directories(azzs_ui_presentation_contract
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

azzs_project_target(
  TARGET azzs_ui_presentation_contract
  ROLE ui
)
