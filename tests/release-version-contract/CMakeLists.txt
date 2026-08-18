find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

set(release_version_contract_command
  "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/check-release-version.py"
  --repository-root
  "${PROJECT_SOURCE_DIR}"
  --cmake-command
  "${CMAKE_COMMAND}")

if(CMAKE_GENERATOR MATCHES "^Visual Studio ")
  list(APPEND release_version_contract_command
    --msbuild-command
    "${CMAKE_MAKE_PROGRAM}")
  if(DEFINED CMAKE_RC_COMPILER AND EXISTS "${CMAKE_RC_COMPILER}")
    list(APPEND release_version_contract_command
      --resource-compiler
      "${CMAKE_RC_COMPILER}")
  endif()
endif()

azzs_register_test(
  NAME release.version.contract
  LABELS contract
  COMMAND
    ${release_version_contract_command}
)

set_tests_properties(
  release.version.contract
  PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    SKIP_RETURN_CODE 77
)
