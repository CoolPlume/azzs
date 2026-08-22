if(NOT WIN32)
  return()
endif()

find_program(AZZS_POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)

azzs_register_test(
  NAME installer.build-manifest.contract
  LABELS contract
  COMMAND
    "${AZZS_POWERSHELL_EXECUTABLE}"
    -NoProfile
    -ExecutionPolicy
    Bypass
    -File
    "${CMAKE_CURRENT_SOURCE_DIR}/check-installer-build-manifest.ps1"
    -RepositoryRoot
    "${PROJECT_SOURCE_DIR}"
    -TemporaryRoot
    "${CMAKE_BINARY_DIR}/contract-tmp/installer-build-manifest"
)

set_tests_properties(
  installer.build-manifest.contract
  PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
