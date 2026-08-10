find_package(Threads REQUIRED)

add_library(azzs_application STATIC
  src/bounded_cpu_scheduler.cpp
  src/workbench.cpp
)

add_library(azzs::application ALIAS azzs_application)

target_include_directories(azzs_application
  PUBLIC
    include
)

target_link_libraries(azzs_application
  PUBLIC
    azzs::domain
    Threads::Threads
)
