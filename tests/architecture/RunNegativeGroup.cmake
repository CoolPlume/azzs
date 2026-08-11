cmake_minimum_required(VERSION 4.1.2)

foreach(required_variable IN ITEMS
    AZZS_NEGATIVE_GROUP
    AZZS_ARCHITECTURE_VALIDATOR
    AZZS_ARCHITECTURE_FIXTURE_DIR
    AZZS_ARCHITECTURE_PROJECT_FIXTURE
    AZZS_ARCHITECTURE_BINARY_DIR
    AZZS_GUARDRAILS_MODULE_DIR
    AZZS_CMAKE_GENERATOR)
  if(NOT DEFINED "${required_variable}")
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

function(expect_architecture_rejection fixture)
  set(expected_fragments ${ARGN})
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}"
      "-DAZZS_GRAPH_FILE=${AZZS_ARCHITECTURE_FIXTURE_DIR}/${fixture}.cmake"
      "-P"
      "${AZZS_ARCHITECTURE_VALIDATOR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE standard_output
    ERROR_VARIABLE standard_error
  )
  set(output "${standard_output}\n${standard_error}")

  if(result EQUAL 0)
    message(FATAL_ERROR
      "Architecture negative '${fixture}' was accepted unexpectedly:\n${output}")
  endif()
  foreach(expected_fragment IN LISTS expected_fragments)
    string(FIND "${output}" "${expected_fragment}" fragment_index)
    if(fragment_index EQUAL -1)
      message(FATAL_ERROR
        "Architecture negative '${fixture}' missed diagnostic '${expected_fragment}':\n${output}")
    endif()
  endforeach()
  message(STATUS "Architecture negative '${fixture}' rejected as expected")
endfunction()

function(expect_cmake_project_rejection scenario)
  set(expected_fragments ${ARGN})
  set(build_directory
    "${AZZS_ARCHITECTURE_BINARY_DIR}/negative-projects/${scenario}")
  file(REMOVE_RECURSE "${build_directory}")

  set(configure_command
    "${CMAKE_COMMAND}"
    -S "${AZZS_ARCHITECTURE_PROJECT_FIXTURE}"
    -B "${build_directory}"
    -G "${AZZS_CMAKE_GENERATOR}"
    "-DAZZS_GUARDRAILS_MODULE_DIR=${AZZS_GUARDRAILS_MODULE_DIR}"
    "-DAZZS_NEGATIVE_SCENARIO=${scenario}"
    -DBUILD_TESTING=ON
  )
  if(DEFINED AZZS_CMAKE_GENERATOR_PLATFORM AND
     NOT AZZS_CMAKE_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND configure_command -A "${AZZS_CMAKE_GENERATOR_PLATFORM}")
  endif()
  if(DEFINED AZZS_CMAKE_GENERATOR_TOOLSET AND
     NOT AZZS_CMAKE_GENERATOR_TOOLSET STREQUAL "")
    list(APPEND configure_command -T "${AZZS_CMAKE_GENERATOR_TOOLSET}")
  endif()
  if(DEFINED AZZS_CMAKE_GENERATOR_INSTANCE AND
     NOT AZZS_CMAKE_GENERATOR_INSTANCE STREQUAL "")
    list(APPEND configure_command
      "-DCMAKE_GENERATOR_INSTANCE=${AZZS_CMAKE_GENERATOR_INSTANCE}")
  endif()

  execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
  )
  set(output "${configure_output}\n${configure_error}")
  set(result "${configure_result}")

  if(configure_result EQUAL 0)
    execute_process(
      COMMAND
        "${CMAKE_COMMAND}"
        "-DAZZS_GRAPH_FILE=${build_directory}/guardrails/project-graph.cmake"
        -P "${AZZS_ARCHITECTURE_VALIDATOR}"
      RESULT_VARIABLE validation_result
      OUTPUT_VARIABLE validation_output
      ERROR_VARIABLE validation_error
    )
    set(result "${validation_result}")
    string(APPEND output "\n${validation_output}\n${validation_error}")
  endif()

  if(result EQUAL 0)
    message(FATAL_ERROR
      "CMake architecture negative '${scenario}' was accepted unexpectedly:\n${output}")
  endif()
  foreach(expected_fragment IN LISTS expected_fragments)
    string(FIND "${output}" "${expected_fragment}" fragment_index)
    if(fragment_index EQUAL -1)
      message(FATAL_ERROR
        "CMake architecture negative '${scenario}' missed diagnostic '${expected_fragment}':\n${output}")
    endif()
  endforeach()
  message(STATUS
    "CMake architecture negative '${scenario}' rejected as expected")
endfunction()

if(AZZS_NEGATIVE_GROUP STREQUAL "reverse-or-cycle")
  expect_architecture_rejection(
    reverse-dependency
    "target: domain-feature"
    "actual edge: domain-feature (domain) -> application-core (application)"
    "allowed edges: domain ->"
  )
  expect_architecture_rejection(
    dependency-cycle
    "target: domain-feature"
    "actual edge: dependency cycle: domain-feature -> application-core -> domain-feature"
    "allowed edges: graph must be acyclic"
  )
  expect_cmake_project_rejection(
    source-bypass
    "target: azzs_domain"
    "actual edge: azzs_domain (domain) -> tests/support/rogue.cpp (test_support)"
    "allowed edges: domain target sources -> domain"
  )
  expect_cmake_project_rejection(
    include-bypass
    "target: azzs_domain"
    "actual edge: azzs_domain (domain) -> directory:src/application/include (application)"
    "allowed edges: domain target include directories -> domain"
  )
  expect_cmake_project_rejection(
    imported-cycle
    "actual edge: dependency cycle:"
    "external:threads"
    "azzs_test_support"
    "allowed edges: graph must be acyclic"
  )
  expect_cmake_project_rejection(
    imported-source-bypass
    "target: external:threads"
    "actual edge: external:threads (runtime) -> tests/support/rogue.cpp (test_support)"
    "allowed edges: runtime -> runtime"
  )
elseif(AZZS_NEGATIVE_GROUP STREQUAL
       "test-support-or-composition-bypass")
  expect_architecture_rejection(
    production-links-test-support
    "target: application-core"
    "actual edge: application-core (application) -> fixed-platform (test_support)"
    "allowed edges: application ->"
  )
  expect_architecture_rejection(
    composition-bypass
    "target: settings-page"
    "actual edge: settings-page (ui) -> registry-adapter (platform_adapter)"
    "allowed edges: ui ->"
  )
  expect_cmake_project_rejection(
    raw-test
    "target: bypass.raw"
    "actual edge: bypass.raw -> raw CTest registration"
    "allowed edges: CTest registrations -> azzs_register_test"
  )
  expect_architecture_rejection(
    msbuild-short-include
    "target: src/adapters/ui/winui/Page.cpp"
    "actual edge: src/adapters/ui/winui/Page.cpp (ui) -> src/adapters/windows/include/windows_platform_info.hpp (platform_adapter)"
    "allowed edges: ui ->"
  )
  expect_architecture_rejection(
    msbuild-dynamic-source
    "target: Azzs.WinUI"
    "actual edge: Azzs.WinUI -> $(RogueSource) (dynamic ClCompile source)"
    "allowed edges: Azzs.WinUI sources ->"
  )
  expect_architecture_rejection(
    msbuild-import
    "target: Azzs.WinUI"
    "actual edge: Azzs.WinUI -> rogue.props (unregistered MSBuild import)"
    "allowed edges: Azzs.WinUI imports ->"
  )
  expect_architecture_rejection(
    msbuild-conditional
    "target: Azzs.WinUI"
    "actual edge: Azzs.WinUI -> dependencies under"
    "allowed edges: Azzs.WinUI dependencies -> unconditional <ItemDefinitionGroup> ancestor"
  )
  expect_architecture_rejection(
    msbuild-conditioned-item
    "target: Azzs.WinUI"
    "actual edge: Azzs.WinUI -> sources under"
    "allowed edges: Azzs.WinUI sources -> unconditional <ItemGroup> ancestor"
  )
  expect_architecture_rejection(
    msbuild-excluded-source
    "target: Azzs.WinUI"
    "actual edge: Azzs.WinUI -> ExcludedFromBuild"
    "allowed edges: Azzs.WinUI sources -> one unconditional source set"
  )
  expect_architecture_rejection(
    msbuild-missing-edge
    "target: Azzs.WinUI"
    "actual edge: Azzs.WinUI -> missing required edge azzs_windows_adapter"
    "allowed edges: Azzs.WinUI ->"
  )
  expect_architecture_rejection(
    msbuild-package
    "target: Azzs.WinUI"
    "actual edge: Azzs.WinUI -> Rogue.Build|1.0.0"
    "allowed edges: Azzs.WinUI packages ->"
  )
  expect_architecture_rejection(
    source-inline-import
    "target: src/adapters/ui/winui/Inline.cpp"
    "actual edge: src/adapters/ui/winui/Inline.cpp (ui) -> module azzs.fixture; import rogue.module;"
    "allowed edges: ui -> registered #include dependencies only"
  )
  expect_architecture_rejection(
    ui-system-effect
    "target: src/adapters/ui/winui/Effect.cpp"
    "actual edge: src/adapters/ui/winui/Effect.cpp (ui) -> direct system effect"
    "allowed edges: ui -> application interfaces for system effects"
  )
else()
  message(FATAL_ERROR
    "Unknown AZZS_NEGATIVE_GROUP '${AZZS_NEGATIVE_GROUP}'")
endif()
