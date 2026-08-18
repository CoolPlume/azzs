find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

azzs_register_test(
  NAME release.version.contract
  LABELS contract
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/check-release-version.py"
    --repository-root
    "${PROJECT_SOURCE_DIR}"
    --cmake-command
    "${CMAKE_COMMAND}"
)

set_tests_properties(
  release.version.contract
  PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
