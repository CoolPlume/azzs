if(NOT WIN32)
  return()
endif()

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

azzs_register_test(
  NAME runtime-catalog-lock.contract
  LABELS contract
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/check_runtime_catalog_lock.py"
    --repository-root
    "${PROJECT_SOURCE_DIR}"
)
