find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

azzs_register_test(
  NAME product.identity.contract
  LABELS contract
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/check-product-identity.py"
    --repository-root
    "${PROJECT_SOURCE_DIR}"
)

set_tests_properties(
  product.identity.contract
  PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
