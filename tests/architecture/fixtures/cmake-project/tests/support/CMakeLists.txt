add_library(azzs_test_support INTERFACE)
add_library(azzs::test_support ALIAS azzs_test_support)

target_include_directories(azzs_test_support INTERFACE include)

azzs_project_target(
  TARGET azzs_test_support
  ROLE test_support
  INTERFACE_LINKS azzs::application
)
