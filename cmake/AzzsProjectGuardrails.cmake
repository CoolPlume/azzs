include_guard(GLOBAL)

set(AZZS_GUARDRAILS_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")
include("${AZZS_GUARDRAILS_MODULE_DIR}/guardrails/ArchitectureRules.cmake")

function(_azzs_guardrail_key value output)
  string(MD5 key "${value}")
  set("${output}" "${key}" PARENT_SCOPE)
endfunction()

function(_azzs_guardrail_fail target actual_edge allowed_edges)
  message(FATAL_ERROR
    "architecture violation\n"
    "  target: ${target}\n"
    "  actual edge: ${actual_edge}\n"
    "  allowed edges: ${allowed_edges}")
endfunction()

function(_azzs_guardrail_registered_role target output)
  get_property(records GLOBAL PROPERTY AZZS_GUARDRAIL_TARGETS)
  foreach(record IN LISTS records)
    string(REPLACE "|" ";" fields "${record}")
    list(GET fields 0 registered_target)
    if(registered_target STREQUAL target)
      list(GET fields 1 registered_role)
      set("${output}" "${registered_role}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set("${output}" "" PARENT_SCOPE)
endfunction()

function(_azzs_guardrail_external_target dependency output_name output_role)
  foreach(record IN LISTS AZZS_ARCHITECTURE_EXTERNAL_CMAKE_TARGETS)
    string(REPLACE "|" ";" fields "${record}")
    list(GET fields 0 external_target)
    if(external_target STREQUAL dependency)
      list(GET fields 1 external_name)
      list(GET fields 2 external_role)
      set("${output_name}" "${external_name}" PARENT_SCOPE)
      set("${output_role}" "${external_role}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set("${output_name}" "" PARENT_SCOPE)
  set("${output_role}" "" PARENT_SCOPE)
endfunction()

function(_azzs_guardrail_matches_any value patterns_variable output)
  foreach(pattern IN LISTS "${patterns_variable}")
    if(value MATCHES "${pattern}")
      set("${output}" TRUE PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set("${output}" FALSE PARENT_SCOPE)
endfunction()

function(_azzs_guardrail_collect_targets directory output)
  get_property(local_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  set(all_targets ${local_targets})

  get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(subdirectory IN LISTS subdirectories)
    _azzs_guardrail_collect_targets("${subdirectory}" child_targets)
    list(APPEND all_targets ${child_targets})
  endforeach()

  set("${output}" "${all_targets}" PARENT_SCOPE)
endfunction()

function(_azzs_guardrail_collect_tests directory output)
  get_property(local_tests DIRECTORY "${directory}" PROPERTY TESTS)
  set(all_tests)
  foreach(test_name IN LISTS local_tests)
    list(APPEND all_tests "${directory}|${test_name}")
  endforeach()

  get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(subdirectory IN LISTS subdirectories)
    _azzs_guardrail_collect_tests("${subdirectory}" child_tests)
    list(APPEND all_tests ${child_tests})
  endforeach()

  set("${output}" "${all_tests}" PARENT_SCOPE)
endfunction()

function(_azzs_guardrail_capture_imported_target dependency)
  if(NOT TARGET "${dependency}")
    return()
  endif()

  get_target_property(canonical_target "${dependency}" ALIASED_TARGET)
  if(NOT canonical_target)
    set(canonical_target "${dependency}")
  endif()
  get_target_property(imported "${canonical_target}" IMPORTED)
  if(NOT imported)
    return()
  endif()

  _azzs_guardrail_key("${canonical_target}" target_key)
  get_property(already_captured GLOBAL PROPERTY
    "AZZS_GUARDRAIL_IMPORTED_CAPTURED_${target_key}")
  if(already_captured)
    return()
  endif()
  set_property(GLOBAL PROPERTY
    "AZZS_GUARDRAIL_IMPORTED_CAPTURED_${target_key}" TRUE)

  set_property(GLOBAL PROPERTY
    "AZZS_GUARDRAIL_IMPORTED_BASE_${target_key}"
    "${CMAKE_CURRENT_SOURCE_DIR}")
  foreach(interface_property IN ITEMS
      INTERFACE_LINK_LIBRARIES
      INTERFACE_SOURCES
      INTERFACE_INCLUDE_DIRECTORIES
      INTERFACE_PRECOMPILE_HEADERS
      INTERFACE_COMPILE_OPTIONS
      INTERFACE_LINK_OPTIONS)
    get_target_property(interface_values "${canonical_target}"
      "${interface_property}")
    if(interface_values MATCHES "-NOTFOUND$")
      set(interface_values)
    endif()
    set_property(GLOBAL PROPERTY
      "AZZS_GUARDRAIL_IMPORTED_${interface_property}_${target_key}"
      "${interface_values}")
  endforeach()

  get_property(interface_libraries GLOBAL PROPERTY
    "AZZS_GUARDRAIL_IMPORTED_INTERFACE_LINK_LIBRARIES_${target_key}")
  foreach(interface_library IN LISTS interface_libraries)
    if(interface_library MATCHES "^\\$<LINK_ONLY:(.+)>$" OR
       interface_library MATCHES "^\\$<BUILD_INTERFACE:(.+)>$")
      set(interface_library "${CMAKE_MATCH_1}")
    endif()
    if(TARGET "${interface_library}")
      _azzs_guardrail_capture_imported_target("${interface_library}")
    endif()
  endforeach()
endfunction()

function(_azzs_guardrail_refresh_imported_target dependency)
  if(NOT TARGET "${dependency}")
    return()
  endif()
  get_target_property(canonical_target "${dependency}" ALIASED_TARGET)
  if(NOT canonical_target)
    set(canonical_target "${dependency}")
  endif()
  get_target_property(imported "${canonical_target}" IMPORTED)
  if(NOT imported)
    return()
  endif()

  _azzs_guardrail_key("${canonical_target}" target_key)
  set_property(GLOBAL PROPERTY
    "AZZS_GUARDRAIL_IMPORTED_BASE_${target_key}"
    "${CMAKE_CURRENT_SOURCE_DIR}")
  foreach(interface_property IN ITEMS
      INTERFACE_LINK_LIBRARIES
      INTERFACE_SOURCES
      INTERFACE_INCLUDE_DIRECTORIES
      INTERFACE_PRECOMPILE_HEADERS
      INTERFACE_COMPILE_OPTIONS
      INTERFACE_LINK_OPTIONS)
    get_target_property(interface_values "${canonical_target}"
      "${interface_property}")
    if(interface_values MATCHES "-NOTFOUND$")
      set(interface_values)
    endif()
    set_property(GLOBAL PROPERTY
      "AZZS_GUARDRAIL_IMPORTED_${interface_property}_${target_key}"
      "${interface_values}")
  endforeach()
  get_property(interface_libraries GLOBAL PROPERTY
    "AZZS_GUARDRAIL_IMPORTED_INTERFACE_LINK_LIBRARIES_${target_key}")
  foreach(interface_library IN LISTS interface_libraries)
    if(interface_library MATCHES "^\\$<LINK_ONLY:(.+)>$" OR
       interface_library MATCHES "^\\$<BUILD_INTERFACE:(.+)>$")
      set(interface_library "${CMAKE_MATCH_1}")
    endif()
    _azzs_guardrail_capture_imported_target("${interface_library}")
  endforeach()
endfunction()

# Declares a project target's role and its direct dependencies. If the target is
# present, the same call applies target_link_libraries. OPTIONAL keeps a
# platform-only target in the architecture graph on other hosts.
function(azzs_project_target)
  cmake_parse_arguments(
    ARG
    "OPTIONAL"
    "TARGET;ROLE"
    "PUBLIC_LINKS;PRIVATE_LINKS;INTERFACE_LINKS"
    ${ARGN}
  )

  if(NOT ARG_TARGET OR NOT ARG_ROLE)
    _azzs_guardrail_fail(
      "${ARG_TARGET}"
      "azzs_project_target -> missing TARGET or ROLE"
      "project target registration -> TARGET and ROLE")
  endif()

  list(FIND AZZS_ARCHITECTURE_ROLES "${ARG_ROLE}" role_index)
  if(role_index EQUAL -1)
    _azzs_guardrail_fail(
      "${ARG_TARGET}"
      "${ARG_TARGET} -> unknown role '${ARG_ROLE}'"
      "project target roles -> ${AZZS_ARCHITECTURE_ROLES}")
  endif()

  set(owned_directory "")
  foreach(record IN LISTS AZZS_ARCHITECTURE_ROLE_DIRECTORIES)
    string(REPLACE "|" ";" fields "${record}")
    list(GET fields 0 directory_role)
    if(directory_role STREQUAL ARG_ROLE)
      list(GET fields 1 relative_directory)
      set(owned_directory "${CMAKE_SOURCE_DIR}/${relative_directory}")
      break()
    endif()
  endforeach()
  if(NOT owned_directory)
    _azzs_guardrail_fail(
      "${ARG_TARGET}"
      "${ARG_TARGET} -> external role '${ARG_ROLE}'"
      "project target roles -> repository-owned role directories")
  endif()
  if(TARGET "${ARG_TARGET}")
    get_target_property(target_source_directory "${ARG_TARGET}" SOURCE_DIR)
  else()
    set(target_source_directory "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  cmake_path(IS_PREFIX owned_directory "${target_source_directory}"
    NORMALIZE role_owns_target)
  if(NOT role_owns_target)
    _azzs_guardrail_fail(
      "${ARG_TARGET}"
      "${ARG_TARGET} (${ARG_ROLE}) -> ${target_source_directory} (unowned directory)"
      "${ARG_ROLE} target directory -> ${owned_directory}")
  endif()
  if(ARG_ROLE STREQUAL "composition" AND
     NOT ARG_TARGET IN_LIST AZZS_ARCHITECTURE_COMPOSITION_NODES)
    _azzs_guardrail_fail(
      "${ARG_TARGET}"
      "${ARG_TARGET} -> unregistered composition role"
      "composition -> ${AZZS_ARCHITECTURE_COMPOSITION_NODES}")
  endif()

  _azzs_guardrail_registered_role("${ARG_TARGET}" existing_role)
  if(existing_role)
    _azzs_guardrail_fail(
      "${ARG_TARGET}"
      "${ARG_TARGET} -> duplicate roles '${existing_role}' and '${ARG_ROLE}'"
      "project targets -> exactly one role")
  endif()

  if(TARGET "${ARG_TARGET}")
    if(ARG_PUBLIC_LINKS)
      target_link_libraries("${ARG_TARGET}" PUBLIC ${ARG_PUBLIC_LINKS})
    endif()
    if(ARG_PRIVATE_LINKS)
      target_link_libraries("${ARG_TARGET}" PRIVATE ${ARG_PRIVATE_LINKS})
    endif()
    if(ARG_INTERFACE_LINKS)
      target_link_libraries("${ARG_TARGET}" INTERFACE ${ARG_INTERFACE_LINKS})
    endif()
    set_property(TARGET "${ARG_TARGET}"
      PROPERTY AZZS_GUARDRAIL_ROLE "${ARG_ROLE}")
  elseif(NOT ARG_OPTIONAL)
    _azzs_guardrail_fail(
      "${ARG_TARGET}"
      "${ARG_TARGET} -> missing build target"
      "project target registration -> existing target or OPTIONAL platform target")
  endif()

  set_property(GLOBAL APPEND PROPERTY AZZS_GUARDRAIL_TARGETS
    "${ARG_TARGET}|${ARG_ROLE}")

  _azzs_guardrail_key("${ARG_TARGET}" target_key)
  set_property(GLOBAL PROPERTY "AZZS_GUARDRAIL_LOGICAL_LINKS_${target_key}"
    "${ARG_PUBLIC_LINKS};${ARG_PRIVATE_LINKS};${ARG_INTERFACE_LINKS}")

  foreach(dependency IN LISTS
      ARG_PUBLIC_LINKS ARG_PRIVATE_LINKS ARG_INTERFACE_LINKS)
    _azzs_guardrail_capture_imported_target("${dependency}")
    cmake_language(DEFER CALL
      _azzs_guardrail_refresh_imported_target "${dependency}")
  endforeach()
endfunction()

# The only CTest registration seam. Owner suites use TARGET; framework checks
# use COMMAND. Every test must declare one or more canonical labels.
function(azzs_register_test)
  cmake_parse_arguments(ARG "" "NAME;TARGET" "LABELS;COMMAND" ${ARGN})

  if(NOT ARG_NAME OR NOT ARG_LABELS)
    _azzs_guardrail_fail(
      "${ARG_NAME}"
      "azzs_register_test -> missing NAME or LABELS"
      "CTest registration -> NAME and canonical LABELS")
  endif()
  if((ARG_TARGET AND ARG_COMMAND) OR (NOT ARG_TARGET AND NOT ARG_COMMAND))
    _azzs_guardrail_fail(
      "${ARG_NAME}"
      "${ARG_NAME} -> ambiguous or missing CTest command"
      "CTest registration -> exactly one of TARGET or COMMAND")
  endif()

  foreach(label IN LISTS ARG_LABELS)
    if(NOT label IN_LIST AZZS_GUARDRAIL_TEST_LABELS)
      _azzs_guardrail_fail(
        "${ARG_NAME}"
        "${ARG_NAME} -> non-canonical CTest label '${label}'"
        "CTest labels -> ${AZZS_GUARDRAIL_TEST_LABELS}")
    endif()
  endforeach()

  if(ARG_TARGET)
    if(NOT TARGET "${ARG_TARGET}")
      _azzs_guardrail_fail(
        "${ARG_NAME}"
        "${ARG_NAME} -> missing test target '${ARG_TARGET}'"
        "CTest target -> registered ROLE test target")
    endif()
    get_target_property(target_role "${ARG_TARGET}" AZZS_GUARDRAIL_ROLE)
    if(NOT target_role STREQUAL "test")
      _azzs_guardrail_fail(
        "${ARG_NAME}"
        "${ARG_NAME} -> ${ARG_TARGET} (${target_role})"
        "CTest target -> ROLE test")
    endif()
    add_test(NAME "${ARG_NAME}" COMMAND "$<TARGET_FILE:${ARG_TARGET}>")
  else()
    add_test(NAME "${ARG_NAME}" COMMAND ${ARG_COMMAND})
  endif()

  set_tests_properties("${ARG_NAME}" PROPERTIES LABELS "${ARG_LABELS}")
  set_property(GLOBAL APPEND PROPERTY AZZS_GUARDRAIL_TESTS
    "${CMAKE_CURRENT_SOURCE_DIR}|${ARG_NAME}")
endfunction()

set(AZZS_GUARDRAIL_TEST_LABELS headless contract architecture)

function(azzs_finalize_project_guardrails)
  get_property(finalization_requested GLOBAL PROPERTY
    AZZS_GUARDRAILS_FINALIZATION_REQUESTED)
  if(finalization_requested)
    _azzs_guardrail_fail(
      "azzs_finalize_project_guardrails"
      "project -> duplicate guardrail finalization request"
      "project guardrails -> exactly one root finalizer")
  endif()
  set_property(GLOBAL PROPERTY
    AZZS_GUARDRAILS_FINALIZATION_REQUESTED TRUE)
  cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
    ID azzs_finalize_project_guardrails
    CALL _azzs_finalize_project_guardrails_impl)
endfunction()

function(_azzs_finalize_project_guardrails_impl)
  get_property(already_finalized GLOBAL PROPERTY AZZS_GUARDRAILS_FINALIZED)
  if(already_finalized)
    _azzs_guardrail_fail(
      "azzs_finalize_project_guardrails"
      "project -> repeated deferred guardrail finalization"
      "project guardrails -> exactly one root finalizer")
  endif()
  set_property(GLOBAL PROPERTY AZZS_GUARDRAILS_FINALIZED TRUE)

  get_property(records GLOBAL PROPERTY AZZS_GUARDRAIL_TARGETS)
  if(NOT records)
    _azzs_guardrail_fail(
      "project"
      "project -> no registered architecture targets"
      "project targets -> azzs_project_target registrations")
  endif()

  set(graph_nodes ${records})
  set(graph_node_names)
  foreach(record IN LISTS records)
    string(REPLACE "|" ";" fields "${record}")
    list(GET fields 0 registered_target)
    list(APPEND graph_node_names "${registered_target}")
  endforeach()

  _azzs_guardrail_collect_targets("${CMAKE_SOURCE_DIR}" build_targets)
  foreach(build_target IN LISTS build_targets)
    get_target_property(imported "${build_target}" IMPORTED)
    get_target_property(target_type "${build_target}" TYPE)
    get_target_property(source_directory "${build_target}" SOURCE_DIR)
    if(imported OR target_type STREQUAL "UTILITY")
      continue()
    endif()
    string(FIND "${source_directory}/" "${CMAKE_SOURCE_DIR}/"
      project_directory_index)
    if(NOT project_directory_index EQUAL 0)
      continue()
    endif()
    list(FIND graph_node_names "${build_target}" registered_index)
    if(registered_index EQUAL -1)
      list(APPEND graph_nodes "${build_target}|unregistered")
      list(APPEND graph_node_names "${build_target}")
    endif()
  endforeach()

  set(graph_edges)
  set(graph_target_directories)
  set(graph_target_sources)
  set(graph_target_include_directories)
  set(graph_target_precompile_headers)
  set(graph_external_sources)
  set(graph_external_include_directories)
  set(graph_external_precompile_headers)
  foreach(record IN LISTS records)
    string(REPLACE "|" ";" fields "${record}")
    list(GET fields 0 registered_target)

    if(TARGET "${registered_target}")
      get_target_property(target_source_directory "${registered_target}"
        SOURCE_DIR)
      list(APPEND graph_target_directories
        "${registered_target}|${target_source_directory}")

      foreach(source_property IN ITEMS SOURCES INTERFACE_SOURCES)
        get_target_property(target_sources "${registered_target}"
          "${source_property}")
        if(target_sources MATCHES "-NOTFOUND$")
          set(target_sources)
        endif()
        foreach(target_source IN LISTS target_sources)
          list(APPEND graph_target_sources
            "${registered_target}|${target_source}")
        endforeach()
      endforeach()

      foreach(include_property IN ITEMS
          INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES)
        get_target_property(target_include_directories "${registered_target}"
          "${include_property}")
        if(target_include_directories MATCHES "-NOTFOUND$")
          set(target_include_directories)
        endif()
        foreach(include_directory IN LISTS target_include_directories)
          list(APPEND graph_target_include_directories
            "${registered_target}|${include_directory}")
        endforeach()
      endforeach()

      foreach(precompile_property IN ITEMS
          PRECOMPILE_HEADERS INTERFACE_PRECOMPILE_HEADERS)
        get_target_property(precompile_headers "${registered_target}"
          "${precompile_property}")
        if(precompile_headers MATCHES "-NOTFOUND$")
          set(precompile_headers)
        endif()
        foreach(precompile_header IN LISTS precompile_headers)
          list(APPEND graph_target_precompile_headers
            "${registered_target}|${precompile_header}")
        endforeach()
      endforeach()

      foreach(option_property IN ITEMS
          COMPILE_OPTIONS INTERFACE_COMPILE_OPTIONS
          LINK_OPTIONS INTERFACE_LINK_OPTIONS)
        get_target_property(target_options "${registered_target}"
          "${option_property}")
        if(target_options MATCHES "-NOTFOUND$")
          set(target_options)
        endif()
        foreach(target_option IN LISTS target_options)
          _azzs_guardrail_matches_any("${target_option}"
            AZZS_ARCHITECTURE_CMAKE_DEPENDENCY_OPTION_PATTERNS
            dependency_option)
          if(dependency_option)
            _azzs_guardrail_fail(
              "${registered_target}"
              "${registered_target} -> ${target_option} (${option_property} dependency injection)"
              "CMake dependencies -> target_sources, target_include_directories, target_precompile_headers, or target_link_libraries")
          endif()
        endforeach()
      endforeach()

      get_target_property(link_libraries "${registered_target}" LINK_LIBRARIES)
      get_target_property(interface_libraries "${registered_target}"
        INTERFACE_LINK_LIBRARIES)
      if(link_libraries MATCHES "-NOTFOUND$")
        set(link_libraries)
      endif()
      if(interface_libraries MATCHES "-NOTFOUND$")
        set(interface_libraries)
      endif()
      set(dependencies ${link_libraries} ${interface_libraries})
    else()
      _azzs_guardrail_key("${registered_target}" target_key)
      get_property(dependencies GLOBAL PROPERTY
        "AZZS_GUARDRAIL_LOGICAL_LINKS_${target_key}")
    endif()

    foreach(dependency IN LISTS dependencies)
      if(dependency STREQUAL "")
        continue()
      endif()
      if(dependency MATCHES "^::@" OR
         dependency MATCHES "^\\$<INSTALL_INTERFACE:")
        continue()
      elseif(dependency MATCHES "^\\$<LINK_ONLY:(.+)>$" OR
             dependency MATCHES "^\\$<BUILD_INTERFACE:(.+)>$")
        set(dependency "${CMAKE_MATCH_1}")
      endif()

      set(canonical_dependency "${dependency}")
      if(TARGET "${dependency}")
        get_target_property(alias_target "${dependency}" ALIASED_TARGET)
        if(alias_target)
          set(canonical_dependency "${alias_target}")
        endif()
      endif()

      _azzs_guardrail_registered_role("${canonical_dependency}" dependency_role)
      if(NOT dependency_role)
        _azzs_guardrail_external_target(
          "${dependency}" external_name external_role)
        if(external_name)
          set(canonical_dependency "${external_name}")
          set(dependency_role "${external_role}")
          list(APPEND imported_dependency_queue
            "${external_name}|${dependency}")
        else()
          set(dependency_role unregistered)
        endif()

        list(FIND graph_node_names "${canonical_dependency}" external_index)
        if(external_index EQUAL -1)
          list(APPEND graph_nodes
            "${canonical_dependency}|${dependency_role}")
          list(APPEND graph_node_names "${canonical_dependency}")
        endif()
      endif()

      list(APPEND graph_edges
        "${registered_target}|${canonical_dependency}")
    endforeach()
  endforeach()

  set(visited_imported_targets)
  while(imported_dependency_queue)
    list(POP_FRONT imported_dependency_queue imported_record)
    string(REPLACE "|" ";" imported_fields "${imported_record}")
    list(GET imported_fields 0 imported_node)
    list(GET imported_fields 1 imported_target)

    if(TARGET "${imported_target}")
      get_target_property(canonical_imported_target "${imported_target}"
        ALIASED_TARGET)
      if(NOT canonical_imported_target)
        set(canonical_imported_target "${imported_target}")
      endif()
    else()
      set(canonical_imported_target "${imported_target}")
    endif()

    list(FIND visited_imported_targets "${canonical_imported_target}"
      imported_visited_index)
    if(NOT imported_visited_index EQUAL -1)
      continue()
    endif()
    list(APPEND visited_imported_targets "${canonical_imported_target}")

    _azzs_guardrail_key("${canonical_imported_target}" imported_target_key)
    get_property(imported_links GLOBAL PROPERTY
      "AZZS_GUARDRAIL_IMPORTED_INTERFACE_LINK_LIBRARIES_${imported_target_key}")
    if(TARGET "${canonical_imported_target}")
      get_target_property(current_imported_links "${canonical_imported_target}"
        INTERFACE_LINK_LIBRARIES)
      if(current_imported_links MATCHES "-NOTFOUND$")
        set(current_imported_links)
      endif()
      if(current_imported_links)
        set(imported_links ${current_imported_links})
      endif()
    endif()

    get_property(imported_base GLOBAL PROPERTY
      "AZZS_GUARDRAIL_IMPORTED_BASE_${imported_target_key}")
    if(NOT imported_base)
      set(imported_base "${CMAKE_SOURCE_DIR}")
    endif()
    foreach(imported_property IN ITEMS
        INTERFACE_SOURCES INTERFACE_INCLUDE_DIRECTORIES
        INTERFACE_PRECOMPILE_HEADERS)
      get_property(imported_values GLOBAL PROPERTY
        "AZZS_GUARDRAIL_IMPORTED_${imported_property}_${imported_target_key}")
      foreach(imported_value IN LISTS imported_values)
        if(imported_property STREQUAL "INTERFACE_SOURCES")
          list(APPEND graph_external_sources
            "${imported_node}|${imported_base}|${imported_value}")
        elseif(imported_property STREQUAL "INTERFACE_INCLUDE_DIRECTORIES")
          list(APPEND graph_external_include_directories
            "${imported_node}|${imported_base}|${imported_value}")
        else()
          list(APPEND graph_external_precompile_headers
            "${imported_node}|${imported_base}|${imported_value}")
        endif()
      endforeach()
    endforeach()
    foreach(imported_option_property IN ITEMS
        INTERFACE_COMPILE_OPTIONS INTERFACE_LINK_OPTIONS)
      get_property(imported_options GLOBAL PROPERTY
        "AZZS_GUARDRAIL_IMPORTED_${imported_option_property}_${imported_target_key}")
      foreach(imported_option IN LISTS imported_options)
        _azzs_guardrail_matches_any("${imported_option}"
          AZZS_ARCHITECTURE_CMAKE_DEPENDENCY_OPTION_PATTERNS
          imported_dependency_option)
        if(imported_dependency_option)
          _azzs_guardrail_fail(
            "${imported_node}"
            "${imported_node} -> ${imported_option} (${imported_option_property} dependency injection)"
            "imported dependencies -> registered literal interface properties")
        endif()
      endforeach()
    endforeach()

    foreach(dependency IN LISTS imported_links)
      if(dependency STREQUAL "" OR dependency MATCHES "^::@" OR
         dependency MATCHES "^\\$<INSTALL_INTERFACE:")
        continue()
      elseif(dependency MATCHES "^\\$<LINK_ONLY:(.+)>$" OR
             dependency MATCHES "^\\$<BUILD_INTERFACE:(.+)>$")
        set(dependency "${CMAKE_MATCH_1}")
      elseif(dependency MATCHES "^\\$<")
        set(canonical_dependency "expression:${dependency}")
        set(dependency_role unregistered)
      else()
        set(canonical_dependency "${dependency}")
        if(TARGET "${dependency}")
          get_target_property(alias_target "${dependency}" ALIASED_TARGET)
          if(alias_target)
            set(canonical_dependency "${alias_target}")
          endif()
        endif()
        _azzs_guardrail_registered_role(
          "${canonical_dependency}" dependency_role)
        if(NOT dependency_role)
          _azzs_guardrail_external_target(
            "${dependency}" external_name external_role)
          if(external_name)
            set(canonical_dependency "${external_name}")
            set(dependency_role "${external_role}")
            list(APPEND imported_dependency_queue
              "${external_name}|${dependency}")
          elseif(TARGET "${dependency}")
            set(dependency_role unregistered)
          else()
            _azzs_guardrail_matches_any("${dependency}"
              AZZS_ARCHITECTURE_EXTERNAL_LINK_ITEM_PATTERNS
              allowed_external_link_item)
            if(allowed_external_link_item)
              continue()
            endif()
            set(canonical_dependency "link-item:${dependency}")
            set(dependency_role unregistered)
          endif()
        endif()
      endif()

      list(FIND graph_node_names "${canonical_dependency}"
        imported_dependency_index)
      if(imported_dependency_index EQUAL -1)
        list(APPEND graph_nodes
          "${canonical_dependency}|${dependency_role}")
        list(APPEND graph_node_names "${canonical_dependency}")
      endif()
      list(APPEND graph_edges
        "${imported_node}|${canonical_dependency}")
    endforeach()
  endwhile()
  list(REMOVE_DUPLICATES graph_edges)
  list(REMOVE_DUPLICATES graph_target_directories)
  list(REMOVE_DUPLICATES graph_target_sources)
  list(REMOVE_DUPLICATES graph_target_include_directories)
  list(REMOVE_DUPLICATES graph_target_precompile_headers)
  list(REMOVE_DUPLICATES graph_external_sources)
  list(REMOVE_DUPLICATES graph_external_include_directories)
  list(REMOVE_DUPLICATES graph_external_precompile_headers)

  set(graph_directory "${CMAKE_BINARY_DIR}/guardrails")
  file(MAKE_DIRECTORY "${graph_directory}")
  set(graph_file "${graph_directory}/project-graph.cmake")
  set(graph_content "set(AZZS_GRAPH_NODES\n")
  foreach(record IN LISTS graph_nodes)
    string(APPEND graph_content "  [==[${record}]==]\n")
  endforeach()
  string(APPEND graph_content ")\nset(AZZS_GRAPH_EDGES\n")
  foreach(record IN LISTS graph_edges)
    string(APPEND graph_content "  [==[${record}]==]\n")
  endforeach()
  string(APPEND graph_content ")\nset(AZZS_GRAPH_TARGET_DIRECTORIES\n")
  foreach(record IN LISTS graph_target_directories)
    string(APPEND graph_content "  [==[${record}]==]\n")
  endforeach()
  string(APPEND graph_content ")\nset(AZZS_GRAPH_TARGET_SOURCES\n")
  foreach(record IN LISTS graph_target_sources)
    string(APPEND graph_content "  [==[${record}]==]\n")
  endforeach()
  string(APPEND graph_content
    ")\nset(AZZS_GRAPH_TARGET_INCLUDE_DIRECTORIES\n")
  foreach(record IN LISTS graph_target_include_directories)
    string(APPEND graph_content "  [==[${record}]==]\n")
  endforeach()
  string(APPEND graph_content
    ")\nset(AZZS_GRAPH_TARGET_PRECOMPILE_HEADERS\n")
  foreach(record IN LISTS graph_target_precompile_headers)
    string(APPEND graph_content "  [==[${record}]==]\n")
  endforeach()
  string(APPEND graph_content ")\nset(AZZS_GRAPH_EXTERNAL_SOURCES\n")
  foreach(record IN LISTS graph_external_sources)
    string(APPEND graph_content "  [==[${record}]==]\n")
  endforeach()
  string(APPEND graph_content
    ")\nset(AZZS_GRAPH_EXTERNAL_INCLUDE_DIRECTORIES\n")
  foreach(record IN LISTS graph_external_include_directories)
    string(APPEND graph_content "  [==[${record}]==]\n")
  endforeach()
  string(APPEND graph_content
    ")\nset(AZZS_GRAPH_EXTERNAL_PRECOMPILE_HEADERS\n")
  foreach(record IN LISTS graph_external_precompile_headers)
    string(APPEND graph_content "  [==[${record}]==]\n")
  endforeach()
  string(APPEND graph_content
    ")\nset(AZZS_GRAPH_SOURCE_ROOT [==[${CMAKE_SOURCE_DIR}]==])\n"
    "set(AZZS_GRAPH_SCAN_PROJECT TRUE)\n")
  file(WRITE "${graph_file}" "${graph_content}")

  execute_process(
    COMMAND
      "${CMAKE_COMMAND}"
      "-DAZZS_GRAPH_FILE=${graph_file}"
      -P
      "${AZZS_GUARDRAILS_MODULE_DIR}/guardrails/ValidateArchitecture.cmake"
    RESULT_VARIABLE validation_result
    OUTPUT_VARIABLE validation_output
    ERROR_VARIABLE validation_error
  )
  if(NOT validation_result STREQUAL "0")
    message(FATAL_ERROR
      "Architecture validator rejected the generated project graph:\n"
      "${validation_output}\n${validation_error}")
  endif()

  if(BUILD_TESTING)
    azzs_register_test(
      NAME architecture.project-graph
      LABELS architecture
      COMMAND
        "${CMAKE_COMMAND}"
        "-DAZZS_GRAPH_FILE=${graph_file}"
        "-P"
        "${AZZS_GUARDRAILS_MODULE_DIR}/guardrails/ValidateArchitecture.cmake"
    )

    get_property(registered_tests GLOBAL PROPERTY AZZS_GUARDRAIL_TESTS)
    _azzs_guardrail_collect_tests("${CMAKE_SOURCE_DIR}" actual_tests)
    foreach(test_record IN LISTS actual_tests)
      string(REPLACE "|" ";" test_fields "${test_record}")
      list(GET test_fields 0 test_directory)
      list(GET test_fields 1 test_name)
      list(FIND registered_tests "${test_record}" registered_test_index)
      if(registered_test_index EQUAL -1)
        _azzs_guardrail_fail(
          "${test_name}"
          "${test_name} -> raw CTest registration in ${test_directory}"
          "CTest registrations -> azzs_register_test with headless, contract, or architecture labels")
      endif()

      get_property(test_labels TEST "${test_name}"
        DIRECTORY "${test_directory}" PROPERTY LABELS)
      if(NOT test_labels)
        _azzs_guardrail_fail(
          "${test_name}"
          "${test_name} -> no canonical CTest label"
          "CTest labels -> ${AZZS_GUARDRAIL_TEST_LABELS}")
      endif()
      foreach(test_label IN LISTS test_labels)
        if(NOT test_label IN_LIST AZZS_GUARDRAIL_TEST_LABELS)
          _azzs_guardrail_fail(
            "${test_name}"
            "${test_name} -> non-canonical CTest label '${test_label}'"
            "CTest labels -> ${AZZS_GUARDRAIL_TEST_LABELS}")
        endif()
      endforeach()
    endforeach()
  endif()
endfunction()
