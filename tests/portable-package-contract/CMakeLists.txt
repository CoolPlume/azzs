if(NOT WIN32)
  return()
endif()

find_program(AZZS_POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)

azzs_register_test(
  NAME portable.package.contract
  LABELS contract
  COMMAND
    "${AZZS_POWERSHELL_EXECUTABLE}"
    -NoProfile
    -ExecutionPolicy
    Bypass
    -File
    "${CMAKE_CURRENT_SOURCE_DIR}/check-portable-package.ps1"
    -RepositoryRoot
    "${PROJECT_SOURCE_DIR}"
)

set_tests_properties(
  portable.package.contract
  PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
