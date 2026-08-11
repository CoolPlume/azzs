add_executable(azzs_core_smoke
  main.cpp
)

target_include_directories(azzs_core_smoke
  PRIVATE
    ../support/include
)

target_link_libraries(azzs_core_smoke
  PRIVATE
    azzs::application
)

add_test(
  NAME core.smoke
  COMMAND azzs_core_smoke
)
