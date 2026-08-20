cmake_minimum_required(VERSION 4.1.2)

if(NOT DEFINED AZZS_GRAPH_FILE OR NOT EXISTS "${AZZS_GRAPH_FILE}")
  message(FATAL_ERROR "AZZS_GRAPH_FILE must name an existing graph fixture")
endif()

include("${AZZS_GRAPH_FILE}")
include("${CMAKE_CURRENT_LIST_DIR}/ArchitectureRules.cmake")

function(_azzs_architecture_fail target actual_edge allowed_edges)
  message(FATAL_ERROR
    "architecture violation\n"
    "  target: ${target}\n"
    "  actual edge: ${actual_edge}\n"
    "  allowed edges: ${allowed_edges}")
endfunction()

function(_azzs_graph_key value output)
  string(MD5 key "${value}")
  set("${output}" "${key}" PARENT_SCOPE)
endfunction()

macro(_azzs_graph_add_node node_name node_role)
  _azzs_graph_key("${node_name}" node_key)
  set(name_variable "AZZS_GRAPH_NAME_${node_key}")
  set(role_variable "AZZS_GRAPH_ROLE_${node_key}")
  if(DEFINED "${name_variable}")
    if(NOT "${${role_variable}}" STREQUAL "${node_role}")
      _azzs_architecture_fail(
        "${node_name}"
        "${node_name} -> conflicting roles '${${role_variable}}' and '${node_role}'"
        "one role per graph node")
    endif()
  else()
    set("${name_variable}" "${node_name}")
    set("${role_variable}" "${node_role}")
    list(APPEND graph_node_keys "${node_key}")
  endif()
endmacro()

macro(_azzs_graph_add_edge from_name to_name)
  list(APPEND graph_edges "${from_name}|${to_name}")
endmacro()

function(_azzs_parse_pair record output_left output_right)
  string(REPLACE "|" ";" fields "${record}")
  list(LENGTH fields field_count)
  if(NOT field_count EQUAL 2)
    message(FATAL_ERROR "Invalid graph record '${record}'; expected left|right")
  endif()
  list(GET fields 0 left)
  list(GET fields 1 right)
  set("${output_left}" "${left}" PARENT_SCOPE)
  set("${output_right}" "${right}" PARENT_SCOPE)
endfunction()

function(_azzs_parse_triple record output_first output_second output_third)
  string(REPLACE "|" ";" fields "${record}")
  list(LENGTH fields field_count)
  if(NOT field_count EQUAL 3)
    message(FATAL_ERROR "Invalid graph record '${record}'; expected first|second|third")
  endif()
  list(GET fields 0 first)
  list(GET fields 1 second)
  list(GET fields 2 third)
  set("${output_first}" "${first}" PARENT_SCOPE)
  set("${output_second}" "${second}" PARENT_SCOPE)
  set("${output_third}" "${third}" PARENT_SCOPE)
endfunction()

function(_azzs_source_role relative_path output)
  string(REPLACE "\\" "/" normalized_path "${relative_path}")

  if(normalized_path IN_LIST AZZS_ARCHITECTURE_COMPOSITION_NODES)
    set("${output}" composition PARENT_SCOPE)
    return()
  endif()
  foreach(record IN LISTS AZZS_ARCHITECTURE_SOURCE_ROLE_OVERRIDES)
    _azzs_parse_pair("${record}" override_role override_path)
    if(normalized_path STREQUAL override_path)
      set("${output}" "${override_role}" PARENT_SCOPE)
      return()
    endif()
  endforeach()

  set(best_role unregistered)
  set(best_length -1)
  foreach(record IN LISTS AZZS_ARCHITECTURE_ROLE_DIRECTORIES)
    _azzs_parse_pair("${record}" directory_role relative_directory)
    string(FIND "${normalized_path}/" "${relative_directory}/"
      prefix_index)
    if(prefix_index EQUAL 0)
      string(LENGTH "${relative_directory}" directory_length)
      if(directory_length GREATER best_length)
        set(best_role "${directory_role}")
        set(best_length "${directory_length}")
      endif()
    endif()
  endforeach()
  set("${output}" "${best_role}" PARENT_SCOPE)
endfunction()

function(_azzs_normalize_project_path raw_path base_directory output_path)
  set(candidate "${raw_path}")
  if(candidate MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
    set(candidate "${CMAKE_MATCH_1}")
  elseif(candidate MATCHES "\\$<")
    set("${output_path}" "" PARENT_SCOPE)
    return()
  endif()

  string(REPLACE "\\" "/" candidate "${candidate}")
  get_filename_component(candidate "${candidate}" ABSOLUTE
    BASE_DIR "${base_directory}")
  cmake_path(NORMAL_PATH candidate)
  set("${output_path}" "${candidate}" PARENT_SCOPE)
endfunction()

function(_azzs_project_relative_role absolute_path output_relative output_role)
  cmake_path(IS_PREFIX AZZS_GRAPH_SOURCE_ROOT "${absolute_path}"
    NORMALIZE path_is_in_project)
  if(path_is_in_project)
    file(RELATIVE_PATH relative_path
      "${AZZS_GRAPH_SOURCE_ROOT}" "${absolute_path}")
    string(REPLACE "\\" "/" relative_path "${relative_path}")
    _azzs_source_role("${relative_path}" path_role)
  else()
    set(relative_path "external:${absolute_path}")
    set(path_role unregistered)
  endif()
  set("${output_relative}" "${relative_path}" PARENT_SCOPE)
  set("${output_role}" "${path_role}" PARENT_SCOPE)
endfunction()

function(_azzs_matches_any value patterns_variable output)
  foreach(pattern IN LISTS "${patterns_variable}")
    if(value MATCHES "${pattern}")
      set("${output}" TRUE PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set("${output}" FALSE PARENT_SCOPE)
endfunction()

function(_azzs_xml_normalize_opening opening output)
  set(normalized "${opening}")
  string(REPLACE "\r" " " normalized "${normalized}")
  string(REPLACE "\n" " " normalized "${normalized}")
  string(REPLACE "\t" " " normalized "${normalized}")
  string(REGEX REPLACE " +" " " normalized "${normalized}")
  string(STRIP "${normalized}" normalized)
  set("${output}" "${normalized}" PARENT_SCOPE)
endfunction()

function(_azzs_xml_enclosing_opening content element tag output)
  string(FIND "${content}" "${element}" element_index)
  if(element_index EQUAL -1)
    set("${output}" "" PARENT_SCOPE)
    return()
  endif()
  string(SUBSTRING "${content}" 0 "${element_index}" prefix)
  set(opening_index -1)
  foreach(opening_marker IN ITEMS
      "<${tag}>" "<${tag} " "<${tag}\t" "<${tag}\r" "<${tag}\n")
    string(FIND "${prefix}" "${opening_marker}" candidate_index REVERSE)
    if(candidate_index GREATER opening_index)
      set(opening_index "${candidate_index}")
    endif()
  endforeach()
  string(FIND "${prefix}" "</${tag}>" closing_index REVERSE)
  if(opening_index EQUAL -1 OR opening_index LESS closing_index)
    set("${output}" "" PARENT_SCOPE)
    return()
  endif()
  string(SUBSTRING "${prefix}" "${opening_index}" -1 opening_tail)
  string(FIND "${opening_tail}" ">" opening_end)
  if(opening_end EQUAL -1)
    set("${output}" "" PARENT_SCOPE)
    return()
  endif()
  math(EXPR opening_length "${opening_end} + 1")
  string(SUBSTRING "${opening_tail}" 0 "${opening_length}" opening)
  _azzs_xml_normalize_opening("${opening}" normalized_opening)
  set("${output}" "${normalized_opening}" PARENT_SCOPE)
endfunction()

function(_azzs_xml_require_plain_ancestor content element tag description)
  _azzs_xml_enclosing_opening(
    "${content}" "${element}" "${tag}" ancestor_opening)
  if(NOT ancestor_opening STREQUAL "<${tag}>")
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> ${description} under '${ancestor_opening}' (conditional or dynamic MSBuild ancestor)"
      "Azzs.WinUI ${description} -> unconditional <${tag}> ancestor")
  endif()
endfunction()

function(_azzs_xml_reject_enclosing_tag content element tag description)
  _azzs_xml_enclosing_opening(
    "${content}" "${element}" "${tag}" ancestor_opening)
  if(ancestor_opening)
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> ${description} under '${ancestor_opening}' (dynamic MSBuild control flow)"
      "Azzs.WinUI ${description} -> unconditional project declaration")
  endif()
endfunction()

function(_azzs_parse_external_record record output_owner output_base output_value)
  string(REPLACE "|" ";" fields "${record}")
  list(LENGTH fields field_count)
  if(field_count EQUAL 2)
    list(GET fields 0 owner)
    list(GET fields 1 value)
    set(base "${AZZS_GRAPH_SOURCE_ROOT}")
  elseif(field_count EQUAL 3)
    list(GET fields 0 owner)
    list(GET fields 1 base)
    list(GET fields 2 value)
  else()
    message(FATAL_ERROR
      "Invalid external graph record '${record}'; expected owner|value or owner|base|value")
  endif()
  set("${output_owner}" "${owner}" PARENT_SCOPE)
  set("${output_base}" "${base}" PARENT_SCOPE)
  set("${output_value}" "${value}" PARENT_SCOPE)
endfunction()

function(_azzs_classify_external_path raw_value base_directory
    output_path output_state)
  set(candidate "${raw_value}")
  if(candidate MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
    set(candidate "${CMAKE_MATCH_1}")
  elseif(candidate MATCHES "^\\$<INSTALL_INTERFACE:")
    set("${output_path}" "" PARENT_SCOPE)
    set("${output_state}" external PARENT_SCOPE)
    return()
  elseif(candidate MATCHES "\\$<")
    set("${output_path}" "${candidate}" PARENT_SCOPE)
    set("${output_state}" unregistered PARENT_SCOPE)
    return()
  endif()

  if(candidate MATCHES "^<[^>]+>$")
    set("${output_path}" "" PARENT_SCOPE)
    set("${output_state}" external PARENT_SCOPE)
    return()
  endif()
  if(candidate MATCHES "^\"(.*)\"$")
    set(candidate "${CMAKE_MATCH_1}")
  endif()

  _azzs_normalize_project_path(
    "${candidate}" "${base_directory}" normalized_candidate)
  if(NOT normalized_candidate)
    set("${output_path}" "${candidate}" PARENT_SCOPE)
    set("${output_state}" unregistered PARENT_SCOPE)
    return()
  endif()
  cmake_path(IS_PREFIX AZZS_GRAPH_SOURCE_ROOT "${normalized_candidate}"
    NORMALIZE candidate_is_project)
  if(candidate_is_project)
    set(state project)
  else()
    set(state external)
  endif()
  set("${output_path}" "${normalized_candidate}" PARENT_SCOPE)
  set("${output_state}" "${state}" PARENT_SCOPE)
endfunction()

macro(_azzs_add_external_path_edge owner base raw_value kind)
  _azzs_classify_external_path(
    "${raw_value}" "${base}" external_path external_path_state)
  if(external_path_state STREQUAL "project")
    _azzs_project_relative_role(
      "${external_path}" external_relative external_role)
    if("${kind}" STREQUAL "include")
      set(external_node "directory:${external_relative}")
    else()
      set(external_node "${external_relative}")
    endif()
    _azzs_graph_add_node("${external_node}" "${external_role}")
    _azzs_graph_add_edge("${owner}" "${external_node}")
  elseif(external_path_state STREQUAL "unregistered")
    set(external_node "external-interface:${raw_value}")
    _azzs_graph_add_node("${external_node}" unregistered)
    _azzs_graph_add_edge("${owner}" "${external_node}")
  endif()
endmacro()

function(_azzs_resolve_project_include source_path include_path source_role output)
  get_filename_component(source_directory "${source_path}" DIRECTORY)
  set(search_directories "${source_directory}")

  file(RELATIVE_PATH relative_source
    "${AZZS_GRAPH_SOURCE_ROOT}" "${source_path}")
  string(REPLACE "\\" "/" relative_source "${relative_source}")
  _azzs_graph_key("${relative_source}" source_key)
  if(DEFINED "AZZS_GRAPH_SOURCE_TARGET_${source_key}")
    set(source_target "${AZZS_GRAPH_SOURCE_TARGET_${source_key}}")
    _azzs_graph_key("${source_target}" source_target_key)
    list(APPEND search_directories
      ${AZZS_GRAPH_TARGET_INCLUDES_${source_target_key}})
  endif()

  _azzs_graph_key("role:${source_role}" source_role_key)
  list(APPEND search_directories ${AZZS_GRAPH_ROLE_INCLUDES_${source_role_key}})
  if(source_role IN_LIST AZZS_ARCHITECTURE_NO_DIRECT_SYSTEM_EFFECT_ROLES)
    list(APPEND search_directories
      "${winui_project_directory}" ${winui_include_directories})
  endif()
  list(APPEND search_directories ${project_include_directories})
  list(REMOVE_DUPLICATES search_directories)

  foreach(search_directory IN LISTS search_directories)
    get_filename_component(candidate
      "${search_directory}/${include_path}" ABSOLUTE)
    cmake_path(NORMAL_PATH candidate)
    if(EXISTS "${candidate}")
      set("${output}" "${candidate}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set("${output}" "" PARENT_SCOPE)
endfunction()

function(_azzs_find_cycle current_key start_key path_keys)
  get_property(found_cycle GLOBAL PROPERTY AZZS_FOUND_CYCLE)
  if(found_cycle)
    return()
  endif()

  foreach(neighbor_key IN LISTS AZZS_GRAPH_ADJACENCY_${current_key})
    if(neighbor_key STREQUAL start_key)
      set(cycle_keys ${path_keys} "${start_key}")
      set_property(GLOBAL PROPERTY AZZS_FOUND_CYCLE "${cycle_keys}")
      return()
    endif()
    if(neighbor_key IN_LIST path_keys)
      continue()
    endif()
    set(next_path ${path_keys} "${neighbor_key}")
    _azzs_find_cycle("${neighbor_key}" "${start_key}" "${next_path}")
    get_property(found_cycle GLOBAL PROPERTY AZZS_FOUND_CYCLE)
    if(found_cycle)
      return()
    endif()
  endforeach()
endfunction()

set(graph_node_keys)
set(graph_edges)
set(declared_graph_node_names)

foreach(record IN LISTS AZZS_GRAPH_NODES)
  _azzs_parse_pair("${record}" node_name node_role)
  _azzs_graph_add_node("${node_name}" "${node_role}")
  list(APPEND declared_graph_node_names "${node_name}")
endforeach()
foreach(record IN LISTS AZZS_GRAPH_EDGES)
  _azzs_parse_pair("${record}" from_name to_name)
  _azzs_graph_add_edge("${from_name}" "${to_name}")
endforeach()

if(AZZS_GRAPH_SCAN_PROJECT)
  if(NOT IS_DIRECTORY "${AZZS_GRAPH_SOURCE_ROOT}")
    message(FATAL_ERROR
      "AZZS_GRAPH_SOURCE_ROOT must name the project root when scanning")
  endif()
  cmake_path(NORMAL_PATH AZZS_GRAPH_SOURCE_ROOT)

  if(DEFINED AZZS_GRAPH_WINUI_PROJECT)
    set(winui_project "${AZZS_GRAPH_WINUI_PROJECT}")
  else()
    set(winui_project
      "${AZZS_GRAPH_SOURCE_ROOT}/src/adapters/ui/winui/Azzs.WinUI.vcxproj")
  endif()
  if(NOT EXISTS "${winui_project}")
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> missing project file '${winui_project}'"
      "one checked WinUI composition host")
  endif()
  get_filename_component(winui_project "${winui_project}" ABSOLUTE)
  cmake_path(NORMAL_PATH winui_project)
  get_filename_component(winui_project_directory "${winui_project}" DIRECTORY)
  file(READ "${winui_project}" winui_project_content)

  _azzs_graph_add_node("Azzs.WinUI" composition)
  _azzs_graph_add_node("external:cpp-runtime" runtime)
  _azzs_graph_add_node("external:windows-platform" platform_runtime)
  _azzs_graph_add_node("external:winui-generated" ui)

  string(TOLOWER "${winui_project_content}" winui_project_content_lower)
  if(winui_project_content_lower MATCHES
     "<[a-z_][a-z0-9_.-]*:(import|projectreference|packagereference|additionaldependencies|additionalincludedirectories|precompiledheaderfile|additionaloptions|forcedincludefiles|clcompile)([^a-z0-9_.:-])")
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> ${CMAKE_MATCH_0} (prefixed MSBuild element)"
      "Azzs.WinUI MSBuild elements -> unprefixed checked declarations")
  endif()

  foreach(rejected_element IN LISTS
      AZZS_ARCHITECTURE_REJECTED_MSBUILD_COMPILE_DEPENDENCY_ELEMENTS)
    string(TOLOWER "${rejected_element}" rejected_element_lower)
    if(winui_project_content_lower MATCHES
       "<${rejected_element_lower}([^a-z0-9_.:-])")
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> ${rejected_element} (unregistered compiler dependency input)"
        "Azzs.WinUI compiler dependency inputs -> ${AZZS_ARCHITECTURE_ALLOWED_MSBUILD_COMPILE_DEPENDENCY_ELEMENTS}")
    endif()
  endforeach()

  string(REGEX MATCHALL "<additionaloptions[^>]*>"
    additional_option_openings_lower "${winui_project_content_lower}")
  string(REGEX MATCHALL "<AdditionalOptions[^>]*>"
    additional_option_openings "${winui_project_content}")
  string(REGEX MATCHALL
    "<AdditionalOptions>[^<]*</AdditionalOptions>"
    additional_option_elements "${winui_project_content}")
  list(LENGTH additional_option_openings_lower
    additional_option_opening_lower_count)
  list(LENGTH additional_option_openings additional_option_opening_count)
  list(LENGTH additional_option_elements additional_option_element_count)
  if(NOT additional_option_opening_lower_count EQUAL
       additional_option_opening_count OR
     NOT additional_option_opening_count EQUAL
       additional_option_element_count)
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> AdditionalOptions declarations ${additional_option_opening_lower_count}, literal declarations ${additional_option_element_count}"
      "Azzs.WinUI AdditionalOptions -> literal checked compiler options")
  endif()
  foreach(additional_option_element IN LISTS additional_option_elements)
    _azzs_xml_enclosing_opening(
      "${winui_project_content}" "${additional_option_element}"
      ClCompile additional_option_compile_ancestor)
    if(NOT additional_option_compile_ancestor)
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> AdditionalOptions outside ClCompile"
        "Azzs.WinUI AdditionalOptions -> ClCompile metadata only")
    endif()
    foreach(control_tag IN ITEMS Choose When Otherwise Target)
      _azzs_xml_reject_enclosing_tag(
        "${winui_project_content}" "${additional_option_element}"
        "${control_tag}" AdditionalOptions)
    endforeach()
    string(REGEX REPLACE
      "^<AdditionalOptions>([^<]*)</AdditionalOptions>$"
      "\\1" additional_options "${additional_option_element}")
    string(REGEX MATCHALL "%\\(AdditionalOptions\\)"
      inherited_additional_options "${additional_options}")
    list(LENGTH inherited_additional_options
      inherited_additional_option_count)
    if(NOT inherited_additional_option_count EQUAL 1 OR
       NOT additional_options MATCHES
         "%\\(AdditionalOptions\\)[ \t\r\n]*$")
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> AdditionalOptions inheritance '${additional_options}'"
        "Azzs.WinUI AdditionalOptions -> one final %(AdditionalOptions) terminator")
    endif()
    string(REGEX REPLACE
      "[ \t\r\n]*%\\(AdditionalOptions\\)[ \t\r\n]*$" ""
      literal_additional_options "${additional_options}")
    foreach(dependency_pattern IN LISTS
        AZZS_ARCHITECTURE_CMAKE_DEPENDENCY_OPTION_PATTERNS)
      if(literal_additional_options MATCHES "${dependency_pattern}")
        string(STRIP "${CMAKE_MATCH_0}" dependency_option)
        _azzs_architecture_fail(
          "Azzs.WinUI"
          "Azzs.WinUI -> AdditionalOptions dependency option '${dependency_option}'"
          "Azzs.WinUI AdditionalOptions -> non-dependency compiler options")
      endif()
    endforeach()
    if(literal_additional_options MATCHES "[$%@]")
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> AdditionalOptions '${literal_additional_options}' (dynamic compiler options)"
        "Azzs.WinUI AdditionalOptions -> literal checked compiler options")
    endif()
    string(REGEX REPLACE "[ \t\r\n]+" ";"
      additional_option_tokens "${literal_additional_options}")
    foreach(additional_option IN LISTS additional_option_tokens)
      if(additional_option AND
         NOT additional_option IN_LIST
           AZZS_ARCHITECTURE_ALLOWED_MSBUILD_ADDITIONAL_OPTIONS)
        _azzs_architecture_fail(
          "Azzs.WinUI"
          "Azzs.WinUI -> AdditionalOptions '${additional_option}' (unregistered compiler option)"
          "Azzs.WinUI AdditionalOptions -> ${AZZS_ARCHITECTURE_ALLOWED_MSBUILD_ADDITIONAL_OPTIONS}")
      endif()
    endforeach()
  endforeach()

  string(REGEX MATCHALL "<precompiledheaderfile[^>]*>"
    precompiled_header_openings_lower "${winui_project_content_lower}")
  string(REGEX MATCHALL "<PrecompiledHeaderFile[^>]*>"
    precompiled_header_openings "${winui_project_content}")
  string(REGEX MATCHALL
    "<PrecompiledHeaderFile>[^<]*</PrecompiledHeaderFile>"
    precompiled_header_elements "${winui_project_content}")
  list(LENGTH precompiled_header_openings_lower
    precompiled_header_opening_lower_count)
  list(LENGTH precompiled_header_openings precompiled_header_opening_count)
  list(LENGTH precompiled_header_elements precompiled_header_element_count)
  if(NOT precompiled_header_opening_lower_count EQUAL
       precompiled_header_opening_count OR
     NOT precompiled_header_opening_count EQUAL
       precompiled_header_element_count)
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> PrecompiledHeaderFile declarations ${precompiled_header_opening_lower_count}, literal declarations ${precompiled_header_element_count}"
      "Azzs.WinUI PrecompiledHeaderFile -> literal WinUI headers")
  endif()
  foreach(precompiled_header_element IN LISTS precompiled_header_elements)
    _azzs_xml_enclosing_opening(
      "${winui_project_content}" "${precompiled_header_element}"
      ClCompile precompiled_header_compile_ancestor)
    if(NOT precompiled_header_compile_ancestor)
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> PrecompiledHeaderFile outside ClCompile"
        "Azzs.WinUI PrecompiledHeaderFile -> ClCompile metadata only")
    endif()
    foreach(control_tag IN ITEMS Choose When Otherwise Target)
      _azzs_xml_reject_enclosing_tag(
        "${winui_project_content}" "${precompiled_header_element}"
        "${control_tag}" PrecompiledHeaderFile)
    endforeach()
    string(REGEX REPLACE
      "^<PrecompiledHeaderFile>([^<]*)</PrecompiledHeaderFile>$"
      "\\1" precompiled_header "${precompiled_header_element}")
    if(NOT precompiled_header OR precompiled_header MATCHES "[$%@?*;]")
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> PrecompiledHeaderFile '${precompiled_header}' (dynamic compiler dependency)"
        "Azzs.WinUI PrecompiledHeaderFile -> literal WinUI headers")
    endif()
    string(REPLACE "\\" "/" precompiled_header "${precompiled_header}")
    get_filename_component(precompiled_header
      "${winui_project_directory}/${precompiled_header}" ABSOLUTE)
    cmake_path(NORMAL_PATH precompiled_header)
    if(NOT EXISTS "${precompiled_header}")
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> ${precompiled_header} (missing PrecompiledHeaderFile)"
        "Azzs.WinUI PrecompiledHeaderFile -> existing WinUI headers")
    endif()
    _azzs_project_relative_role(
      "${precompiled_header}" precompiled_header_relative
      precompiled_header_role)
    if(NOT precompiled_header_role STREQUAL "ui")
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI (composition) -> ${precompiled_header_relative} (${precompiled_header_role} PrecompiledHeaderFile)"
        "Azzs.WinUI PrecompiledHeaderFile -> ui")
    endif()
    _azzs_graph_add_node(
      "${precompiled_header_relative}" "${precompiled_header_role}")
    _azzs_graph_add_edge("Azzs.WinUI" "${precompiled_header_relative}")
  endforeach()

  file(GLOB_RECURSE directory_build_files LIST_DIRECTORIES FALSE
    "${AZZS_GRAPH_SOURCE_ROOT}/Directory.Build.props"
    "${AZZS_GRAPH_SOURCE_ROOT}/Directory.Build.targets")
  foreach(directory_build_file IN LISTS directory_build_files)
    file(RELATIVE_PATH directory_build_relative
      "${AZZS_GRAPH_SOURCE_ROOT}" "${directory_build_file}")
    string(REPLACE "\\" "/" directory_build_relative
      "${directory_build_relative}")
    _azzs_matches_any("${directory_build_relative}"
      AZZS_ARCHITECTURE_SCAN_EXCLUDE_PATTERNS directory_build_excluded)
    if(directory_build_excluded)
      continue()
    endif()
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> ${directory_build_relative} (implicit MSBuild import)"
      "Azzs.WinUI imports -> explicit registered project imports only")
  endforeach()

  if(winui_project_content MATCHES
     "<ProjectReference([^A-Za-z0-9_.:-])")
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> ProjectReference (unregistered MSBuild dependency source)"
      "Azzs.WinUI dependencies -> one literal AdditionalDependencies list")
  endif()

  string(REGEX MATCHALL "<Import([^A-Za-z0-9_.:-][^>]*)?>" import_elements
    "${winui_project_content}")
  foreach(import_element IN LISTS import_elements)
    _azzs_xml_normalize_opening("${import_element}" normalized_import)
    if(NOT normalized_import MATCHES
       "^<Import[ \\t]+Project=\"([^\"]+)\"[ \\t]*/>$")
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> ${normalized_import} (dynamic MSBuild import)"
        "Azzs.WinUI imports -> ${AZZS_ARCHITECTURE_ALLOWED_MSBUILD_IMPORTS}")
    endif()
    set(import_path "${CMAKE_MATCH_1}")
    if(NOT import_path IN_LIST AZZS_ARCHITECTURE_ALLOWED_MSBUILD_IMPORTS)
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> ${import_path} (unregistered MSBuild import)"
      "Azzs.WinUI imports -> ${AZZS_ARCHITECTURE_ALLOWED_MSBUILD_IMPORTS}")
    endif()
  endforeach()

  string(REGEX MATCHALL
    "<PackageReference([^A-Za-z0-9_.:-][^>]*)?>"
    package_elements "${winui_project_content}")
  set(package_records)
  foreach(package_element IN LISTS package_elements)
    _azzs_xml_normalize_opening("${package_element}" normalized_package)
    if(NOT normalized_package MATCHES
       "^<PackageReference Include=\"([^\"]+)\" Version=\"([^\"]+)\"[ ]*/>$")
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> ${normalized_package} (dynamic PackageReference)"
        "Azzs.WinUI packages -> ${AZZS_ARCHITECTURE_ALLOWED_MSBUILD_PACKAGES}")
    endif()
    set(package_record "${CMAKE_MATCH_1}|${CMAKE_MATCH_2}")
    if(NOT package_record IN_LIST AZZS_ARCHITECTURE_ALLOWED_MSBUILD_PACKAGES OR
       package_record IN_LIST package_records)
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> ${package_record} (unregistered or duplicate PackageReference)"
        "Azzs.WinUI packages -> ${AZZS_ARCHITECTURE_ALLOWED_MSBUILD_PACKAGES}")
    endif()
    _azzs_xml_require_plain_ancestor(
      "${winui_project_content}" "${package_element}" ItemGroup packages)
    foreach(control_tag IN ITEMS Choose When Otherwise Target)
      _azzs_xml_reject_enclosing_tag(
        "${winui_project_content}" "${package_element}"
        "${control_tag}" packages)
    endforeach()
    list(APPEND package_records "${package_record}")
  endforeach()
  list(LENGTH package_records package_count)
  list(LENGTH AZZS_ARCHITECTURE_ALLOWED_MSBUILD_PACKAGES
    expected_package_count)
  if(NOT package_count EQUAL expected_package_count)
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> PackageReference set '${package_records}'"
      "Azzs.WinUI packages -> exactly ${AZZS_ARCHITECTURE_ALLOWED_MSBUILD_PACKAGES}")
  endif()

  string(REGEX MATCHALL "<AdditionalDependencies[^>]*>"
    dependency_openings "${winui_project_content}")
  string(REGEX MATCH
    "<AdditionalDependencies>[^<]*</AdditionalDependencies>"
    dependency_element "${winui_project_content}")
  list(LENGTH dependency_openings dependency_opening_count)
  if(dependency_element)
    set(dependency_element_count 1)
  else()
    set(dependency_element_count 0)
  endif()
  if(NOT dependency_opening_count EQUAL 1 OR
     NOT dependency_element_count EQUAL 1)
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> AdditionalDependencies declarations ${dependency_opening_count}, literal declarations ${dependency_element_count}"
      "Azzs.WinUI dependencies -> exactly one unconditional literal declaration")
  endif()
  _azzs_xml_require_plain_ancestor(
    "${winui_project_content}" "${dependency_element}"
    ItemDefinitionGroup dependencies)
  _azzs_xml_require_plain_ancestor(
    "${winui_project_content}" "${dependency_element}" Link dependencies)
  foreach(control_tag IN ITEMS Choose When Otherwise Target)
    _azzs_xml_reject_enclosing_tag(
      "${winui_project_content}" "${dependency_element}"
      "${control_tag}" dependencies)
  endforeach()
  string(REGEX REPLACE
    "^<AdditionalDependencies>([^<]*)</AdditionalDependencies>$"
    "\\1" dependencies "${dependency_element}")
  set(inherited_dependency_count 0)
  foreach(dependency IN LISTS dependencies)
    string(STRIP "${dependency}" dependency)
    if(dependency STREQUAL "%(AdditionalDependencies)")
      math(EXPR inherited_dependency_count
        "${inherited_dependency_count} + 1")
      continue()
    endif()
    if(NOT dependency OR dependency MATCHES "[$%@]" OR
       dependency MATCHES "[?*]")
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> ${dependency} (dynamic MSBuild dependency)"
        "Azzs.WinUI dependencies -> literal registered libraries")
    endif()

    string(TOLOWER "${dependency}" normalized_dependency)
    string(REGEX REPLACE "\\.lib$" "" project_target
      "${normalized_dependency}")
    _azzs_graph_key("${project_target}" project_target_key)
    if(DEFINED "AZZS_GRAPH_NAME_${project_target_key}")
      _azzs_graph_add_edge("Azzs.WinUI" "${project_target}")
      continue()
    endif()

    set(external_matched FALSE)
    foreach(external_record IN LISTS
        AZZS_ARCHITECTURE_EXTERNAL_MSBUILD_LIBRARIES)
      string(REPLACE "|" ";" external_fields "${external_record}")
      list(GET external_fields 0 external_library)
      if(normalized_dependency STREQUAL external_library)
        list(GET external_fields 1 external_name)
        list(GET external_fields 2 external_role)
        _azzs_graph_add_node("${external_name}" "${external_role}")
        _azzs_graph_add_edge("Azzs.WinUI" "${external_name}")
        set(external_matched TRUE)
        break()
      endif()
    endforeach()
    if(NOT external_matched)
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI (composition) -> ${dependency} (unregistered)"
        "composition -> ${AZZS_ARCHITECTURE_ALLOWED_composition}")
    endif()
  endforeach()
  if(NOT inherited_dependency_count EQUAL 1)
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> inherited AdditionalDependencies count ${inherited_dependency_count}"
      "Azzs.WinUI dependencies -> one %(AdditionalDependencies) terminator")
  endif()
  list(GET dependencies -1 inherited_dependency_terminator)
  string(STRIP "${inherited_dependency_terminator}"
    inherited_dependency_terminator)
  if(NOT inherited_dependency_terminator STREQUAL
     "%(AdditionalDependencies)")
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> inherited AdditionalDependencies is not the final item"
      "Azzs.WinUI dependencies -> one final %(AdditionalDependencies) terminator")
  endif()

  foreach(expected_edge IN LISTS AZZS_ARCHITECTURE_EXPECTED_MSBUILD_EDGES)
    _azzs_parse_pair("${expected_edge}" expected_from expected_to)
    _azzs_graph_key("${expected_to}" expected_to_key)
    if(NOT DEFINED "AZZS_GRAPH_NAME_${expected_to_key}")
      continue()
    endif()
    if(NOT expected_edge IN_LIST graph_edges)
      _azzs_architecture_fail(
        "${expected_from}"
        "${expected_from} -> missing required edge ${expected_to}"
        "${expected_from} -> ${AZZS_ARCHITECTURE_EXPECTED_MSBUILD_EDGES}")
    endif()
  endforeach()

  string(REGEX MATCHALL "<AdditionalIncludeDirectories[^>]*>"
    include_openings "${winui_project_content}")
  string(REGEX MATCH
    "<AdditionalIncludeDirectories>[^<]*</AdditionalIncludeDirectories>"
    include_element "${winui_project_content}")
  list(LENGTH include_openings include_opening_count)
  if(include_element)
    set(include_element_count 1)
  else()
    set(include_element_count 0)
  endif()
  if(NOT include_opening_count EQUAL 1 OR NOT include_element_count EQUAL 1)
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> AdditionalIncludeDirectories declarations ${include_opening_count}, literal declarations ${include_element_count}"
      "Azzs.WinUI include directories -> exactly one unconditional literal declaration")
  endif()
  _azzs_xml_require_plain_ancestor(
    "${winui_project_content}" "${include_element}"
    ItemDefinitionGroup include-directories)
  _azzs_xml_require_plain_ancestor(
    "${winui_project_content}" "${include_element}"
    ClCompile include-directories)
  foreach(control_tag IN ITEMS Choose When Otherwise Target)
    _azzs_xml_reject_enclosing_tag(
      "${winui_project_content}" "${include_element}"
      "${control_tag}" include-directories)
  endforeach()
  string(REGEX REPLACE
    "^<AdditionalIncludeDirectories>([^<]*)</AdditionalIncludeDirectories>$"
    "\\1" include_directories "${include_element}")
  set(winui_include_directories)
  set(inherited_include_count 0)
  set(msbuild_directory_prefix [==[$(MSBuildThisFileDirectory)]==])
  # CMake creates this header directory during configure. Keep its checked
  # project-relative path valid even when the validator runs before the first
  # generated header is materialized; all other missing paths still fail.
  set(generated_header_include_relative "out/obj/winui/generated")
  string(LENGTH "${msbuild_directory_prefix}" msbuild_prefix_length)
  foreach(include_directory IN LISTS include_directories)
    string(STRIP "${include_directory}" include_directory)
    if(include_directory STREQUAL "%(AdditionalIncludeDirectories)")
      math(EXPR inherited_include_count "${inherited_include_count} + 1")
      continue()
    endif()
    string(FIND "${include_directory}" "${msbuild_directory_prefix}"
      msbuild_prefix_index)
    if(NOT msbuild_prefix_index EQUAL 0)
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> ${include_directory} (dynamic or external include directory)"
        "Azzs.WinUI include directories -> $(MSBuildThisFileDirectory) project paths")
    endif()
    string(SUBSTRING "${include_directory}" "${msbuild_prefix_length}" -1
      include_relative)
    string(REPLACE "\\" "/" include_relative "${include_relative}")
    if(include_relative MATCHES "[$%@?*]")
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> ${include_directory} (dynamic include directory)"
        "Azzs.WinUI include directories -> literal project paths")
    endif()
    get_filename_component(include_absolute
      "${winui_project_directory}/${include_relative}" ABSOLUTE)
    cmake_path(NORMAL_PATH include_absolute)
    _azzs_project_relative_role(
      "${include_absolute}" include_project_path include_role)
    if(NOT IS_DIRECTORY "${include_absolute}")
      if(NOT "${include_project_path}" STREQUAL
         "${generated_header_include_relative}" OR
         EXISTS "${include_absolute}")
        _azzs_architecture_fail(
          "Azzs.WinUI"
          "Azzs.WinUI -> ${include_absolute} (missing include directory)"
          "Azzs.WinUI include directories -> existing registered project paths")
      endif()
    endif()
    _azzs_graph_add_node("directory:${include_project_path}" "${include_role}")
    _azzs_graph_add_edge("Azzs.WinUI" "directory:${include_project_path}")
    list(APPEND winui_include_directories "${include_absolute}")
  endforeach()
  if(NOT inherited_include_count EQUAL 1)
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> inherited AdditionalIncludeDirectories count ${inherited_include_count}"
      "Azzs.WinUI include directories -> one %(AdditionalIncludeDirectories) terminator")
  endif()
  list(GET include_directories -1 inherited_include_terminator)
  string(STRIP "${inherited_include_terminator}"
    inherited_include_terminator)
  if(NOT inherited_include_terminator STREQUAL
     "%(AdditionalIncludeDirectories)")
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> inherited AdditionalIncludeDirectories is not the final item"
      "Azzs.WinUI include directories -> one final %(AdditionalIncludeDirectories) terminator")
  endif()

  if(winui_project_content MATCHES
     "<ExcludedFromBuild([^A-Za-z0-9_.:-])")
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> ExcludedFromBuild (configuration-specific source set)"
      "Azzs.WinUI sources -> one unconditional source set for all configurations")
  endif()

  string(REGEX MATCHALL
    "<ClCompile>|<ClCompile[^A-Za-z0-9_.:>/-][^>]*>"
    all_compile_elements
    "${winui_project_content}")
  set(compile_elements)
  foreach(compile_element IN LISTS all_compile_elements)
    _azzs_xml_normalize_opening("${compile_element}" normalized_compile)
    if(NOT normalized_compile STREQUAL "<ClCompile>")
      list(APPEND compile_elements "${compile_element}")
    endif()
  endforeach()
  set(composition_root_count 0)
  set(generated_module_count 0)
  set(declared_compile_sources)
  set(winui_source_root
    "${AZZS_GRAPH_SOURCE_ROOT}/src/adapters/ui/winui")
  set(composition_root
    "${AZZS_GRAPH_SOURCE_ROOT}/src/composition/windows/composition_root.cpp")
  cmake_path(NORMAL_PATH winui_source_root)
  cmake_path(NORMAL_PATH composition_root)

  foreach(compile_element IN LISTS compile_elements)
    _azzs_xml_normalize_opening("${compile_element}" normalized_compile)
    if(NOT normalized_compile MATCHES
       "^<ClCompile[ \\t]+Include=\"([^\"]+)\"[ \\t]*/?>$")
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> ${normalized_compile} (dynamic ClCompile item)"
        "Azzs.WinUI sources -> one literal Include attribute")
    endif()
    set(compile_source "${CMAKE_MATCH_1}")
    if(compile_source IN_LIST declared_compile_sources)
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> ${compile_source} (duplicate ClCompile source)"
        "Azzs.WinUI sources -> each source declared exactly once")
    endif()
    list(APPEND declared_compile_sources "${compile_source}")
    _azzs_xml_require_plain_ancestor(
      "${winui_project_content}" "${compile_element}" ItemGroup sources)
    foreach(control_tag IN ITEMS Choose When Otherwise Target)
      _azzs_xml_reject_enclosing_tag(
        "${winui_project_content}" "${compile_element}"
        "${control_tag}" sources)
    endforeach()
    if(compile_source STREQUAL [==[$(GeneratedFilesDir)module.g.cpp]==])
      math(EXPR generated_module_count "${generated_module_count} + 1")
      continue()
    endif()
    if(compile_source MATCHES "[$%@?*;]")
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> ${compile_source} (dynamic ClCompile source)"
        "Azzs.WinUI sources -> WinUI tree, composition_root.cpp, or $(GeneratedFilesDir)module.g.cpp")
    endif()

    string(REPLACE "\\" "/" compile_source "${compile_source}")
    get_filename_component(compile_source
      "${winui_project_directory}/${compile_source}" ABSOLUTE)
    cmake_path(NORMAL_PATH compile_source)
    if(NOT EXISTS "${compile_source}")
      _azzs_architecture_fail(
        "Azzs.WinUI"
        "Azzs.WinUI -> ${compile_source} (missing ClCompile source)"
        "Azzs.WinUI sources -> existing registered source files")
    endif()

    if(compile_source STREQUAL composition_root)
      math(EXPR composition_root_count "${composition_root_count} + 1")
    else()
      cmake_path(IS_PREFIX winui_source_root "${compile_source}"
        NORMALIZE source_is_winui)
      if(NOT source_is_winui)
        _azzs_architecture_fail(
          "Azzs.WinUI"
          "Azzs.WinUI (composition) -> ${compile_source} (unowned source)"
          "Azzs.WinUI sources -> WinUI tree or composition_root.cpp")
      endif()
    endif()
    _azzs_project_relative_role(
      "${compile_source}" compile_relative compile_role)
    _azzs_graph_add_node("${compile_relative}" "${compile_role}")
    _azzs_graph_add_edge("Azzs.WinUI" "${compile_relative}")
  endforeach()
  if(NOT generated_module_count EQUAL 1)
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> generated module count ${generated_module_count}"
      "Azzs.WinUI -> exactly one $(GeneratedFilesDir)module.g.cpp")
  endif()
  if(NOT composition_root_count EQUAL 1)
    _azzs_architecture_fail(
      "Azzs.WinUI"
      "Azzs.WinUI -> composition_root.cpp count ${composition_root_count}"
      "Azzs.WinUI -> exactly one composition_root.cpp")
  endif()

  set(project_include_directories ${winui_include_directories})
  foreach(record IN LISTS AZZS_GRAPH_TARGET_DIRECTORIES)
    _azzs_parse_pair("${record}" target_name target_directory)
    _azzs_graph_key("${target_name}" target_key)
    if(NOT DEFINED "AZZS_GRAPH_NAME_${target_key}")
      _azzs_architecture_fail(
        "${target_name}"
        "${target_name} -> target directory without registered target"
        "target directories -> registered project targets")
    endif()
    _azzs_normalize_project_path(
      "${target_directory}" "${AZZS_GRAPH_SOURCE_ROOT}" normalized_directory)
    if(NOT normalized_directory)
      set(directory_relative "expression:${target_directory}")
      set(directory_role unregistered)
    else()
      _azzs_project_relative_role(
        "${normalized_directory}" directory_relative directory_role)
    endif()
    set(target_role "${AZZS_GRAPH_ROLE_${target_key}}")
    if(NOT directory_role STREQUAL target_role)
      _azzs_architecture_fail(
        "${target_name}"
        "${target_name} (${target_role}) -> directory:${directory_relative} (${directory_role})"
        "${target_role} target directories -> ${target_role}")
    endif()
    _azzs_graph_add_node("directory:${directory_relative}" "${directory_role}")
    _azzs_graph_add_edge("${target_name}" "directory:${directory_relative}")
    set("AZZS_GRAPH_TARGET_SOURCE_DIR_${target_key}"
      "${normalized_directory}")
  endforeach()

  foreach(record IN LISTS AZZS_GRAPH_TARGET_INCLUDE_DIRECTORIES)
    _azzs_parse_pair("${record}" target_name include_directory)
    _azzs_graph_key("${target_name}" target_key)
    set(target_base "${AZZS_GRAPH_TARGET_SOURCE_DIR_${target_key}}")
    if(NOT target_base)
      set(target_base "${AZZS_GRAPH_SOURCE_ROOT}")
    endif()
    _azzs_normalize_project_path(
      "${include_directory}" "${target_base}" normalized_include)
    if(NOT normalized_include OR NOT IS_DIRECTORY "${normalized_include}")
      set(include_relative "expression-or-missing:${include_directory}")
      set(include_role unregistered)
    else()
      _azzs_project_relative_role(
        "${normalized_include}" include_relative include_role)
      list(APPEND project_include_directories "${normalized_include}")
      list(APPEND "AZZS_GRAPH_TARGET_INCLUDES_${target_key}"
        "${normalized_include}")
      _azzs_graph_key("role:${AZZS_GRAPH_ROLE_${target_key}}" target_role_key)
      list(APPEND "AZZS_GRAPH_ROLE_INCLUDES_${target_role_key}"
        "${normalized_include}")
    endif()
    set(target_role "${AZZS_GRAPH_ROLE_${target_key}}")
    if(NOT include_role STREQUAL target_role)
      _azzs_architecture_fail(
        "${target_name}"
        "${target_name} (${target_role}) -> directory:${include_relative} (${include_role})"
        "${target_role} target include directories -> ${target_role}")
    endif()
    _azzs_graph_add_node("directory:${include_relative}" "${include_role}")
    _azzs_graph_add_edge("${target_name}" "directory:${include_relative}")
  endforeach()
  list(REMOVE_DUPLICATES project_include_directories)

  foreach(record IN LISTS AZZS_GRAPH_TARGET_PRECOMPILE_HEADERS)
    _azzs_parse_pair("${record}" target_name precompile_header)
    _azzs_graph_key("${target_name}" target_key)
    if(NOT DEFINED "AZZS_GRAPH_NAME_${target_key}")
      _azzs_architecture_fail(
        "${target_name}"
        "${target_name} -> precompile header without registered target"
        "target precompile headers -> registered project targets")
    endif()

    set(header_value "${precompile_header}")
    if(header_value MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
      set(header_value "${CMAKE_MATCH_1}")
    elseif(header_value MATCHES "^\\$<INSTALL_INTERFACE:")
      continue()
    elseif(header_value MATCHES "\\$<")
      set(header_node "precompile-header:${header_value}")
      _azzs_graph_add_node("${header_node}" unregistered)
      _azzs_graph_add_edge("${target_name}" "${header_node}")
      continue()
    endif()

    set(header_kind path)
    if(header_value MATCHES "^<([^>]+)>$")
      set(header_value "${CMAKE_MATCH_1}")
      set(header_kind angle)
    elseif(header_value MATCHES "^\"(.*)\"$")
      set(header_value "${CMAKE_MATCH_1}")
    endif()
    string(TOLOWER "${header_value}" normalized_header)
    _azzs_matches_any("${normalized_header}"
      AZZS_ARCHITECTURE_PLATFORM_HEADER_PATTERNS platform_header)
    _azzs_matches_any("${normalized_header}"
      AZZS_ARCHITECTURE_STANDARD_HEADER_PATTERNS standard_header)
    if(platform_header)
      _azzs_graph_add_edge(
        "${target_name}" "external:windows-platform")
      continue()
    elseif(standard_header AND header_kind STREQUAL "angle")
      _azzs_graph_add_edge("${target_name}" "external:cpp-runtime")
      continue()
    endif()

    set(header_candidates)
    set(target_base "${AZZS_GRAPH_TARGET_SOURCE_DIR_${target_key}}")
    if(NOT target_base)
      set(target_base "${AZZS_GRAPH_SOURCE_ROOT}")
    endif()
    if(IS_ABSOLUTE "${header_value}")
      list(APPEND header_candidates "${header_value}")
    else()
      list(APPEND header_candidates "${target_base}/${header_value}")
      foreach(include_directory IN LISTS
          AZZS_GRAPH_TARGET_INCLUDES_${target_key})
        list(APPEND header_candidates
          "${include_directory}/${header_value}")
      endforeach()
    endif()
    set(resolved_header "")
    foreach(header_candidate IN LISTS header_candidates)
      get_filename_component(header_candidate "${header_candidate}" ABSOLUTE)
      cmake_path(NORMAL_PATH header_candidate)
      if(EXISTS "${header_candidate}")
        set(resolved_header "${header_candidate}")
        break()
      endif()
    endforeach()
    if(resolved_header)
      _azzs_project_relative_role(
        "${resolved_header}" header_relative header_role)
      _azzs_graph_add_node("${header_relative}" "${header_role}")
      _azzs_graph_add_edge("${target_name}" "${header_relative}")
    else()
      set(header_node "unresolved-precompile-header:${header_value}")
      _azzs_graph_add_node("${header_node}" unregistered)
      _azzs_graph_add_edge("${target_name}" "${header_node}")
    endif()
  endforeach()

  foreach(record IN LISTS AZZS_GRAPH_EXTERNAL_SOURCES)
    _azzs_parse_external_record(
      "${record}" external_owner external_base external_value)
    _azzs_add_external_path_edge(
      "${external_owner}" "${external_base}" "${external_value}" source)
  endforeach()
  foreach(record IN LISTS AZZS_GRAPH_EXTERNAL_INCLUDE_DIRECTORIES)
    _azzs_parse_external_record(
      "${record}" external_owner external_base external_value)
    _azzs_add_external_path_edge(
      "${external_owner}" "${external_base}" "${external_value}" include)
  endforeach()
  foreach(record IN LISTS AZZS_GRAPH_EXTERNAL_PRECOMPILE_HEADERS)
    _azzs_parse_external_record(
      "${record}" external_owner external_base external_value)
    _azzs_add_external_path_edge(
      "${external_owner}" "${external_base}" "${external_value}" precompile)
  endforeach()

  foreach(record IN LISTS AZZS_GRAPH_TARGET_SOURCES)
    _azzs_parse_pair("${record}" target_name target_source)
    _azzs_graph_key("${target_name}" target_key)
    set(target_base "${AZZS_GRAPH_TARGET_SOURCE_DIR_${target_key}}")
    if(NOT target_base)
      set(target_base "${AZZS_GRAPH_SOURCE_ROOT}")
    endif()
    _azzs_normalize_project_path(
      "${target_source}" "${target_base}" normalized_source)
    if(NOT normalized_source OR NOT EXISTS "${normalized_source}")
      set(source_relative "expression-or-missing:${target_source}")
      set(source_role unregistered)
    else()
      _azzs_project_relative_role(
        "${normalized_source}" source_relative source_role)
      _azzs_graph_key("${source_relative}" source_key)
      set("AZZS_GRAPH_SOURCE_TARGET_${source_key}" "${target_name}")
    endif()
    set(target_role "${AZZS_GRAPH_ROLE_${target_key}}")
    if(NOT source_role STREQUAL target_role)
      _azzs_architecture_fail(
        "${target_name}"
        "${target_name} (${target_role}) -> ${source_relative} (${source_role})"
        "${target_role} target sources -> ${target_role}")
    endif()
    _azzs_graph_add_node("${source_relative}" "${source_role}")
    _azzs_graph_add_edge("${target_name}" "${source_relative}")
  endforeach()

  set(project_source_patterns
    "${AZZS_GRAPH_SOURCE_ROOT}/src/*.c"
    "${AZZS_GRAPH_SOURCE_ROOT}/src/*.cc"
    "${AZZS_GRAPH_SOURCE_ROOT}/src/*.cpp"
    "${AZZS_GRAPH_SOURCE_ROOT}/src/*.cppm"
    "${AZZS_GRAPH_SOURCE_ROOT}/src/*.cxx"
    "${AZZS_GRAPH_SOURCE_ROOT}/src/*.h"
    "${AZZS_GRAPH_SOURCE_ROOT}/src/*.hpp"
    "${AZZS_GRAPH_SOURCE_ROOT}/src/*.inl"
    "${AZZS_GRAPH_SOURCE_ROOT}/src/*.ipp"
    "${AZZS_GRAPH_SOURCE_ROOT}/src/*.ixx")
  if(NOT DEFINED AZZS_GRAPH_SCAN_TEST_SOURCES OR
     AZZS_GRAPH_SCAN_TEST_SOURCES)
    list(APPEND project_source_patterns
      "${AZZS_GRAPH_SOURCE_ROOT}/tests/*.c"
      "${AZZS_GRAPH_SOURCE_ROOT}/tests/*.cc"
      "${AZZS_GRAPH_SOURCE_ROOT}/tests/*.cpp"
      "${AZZS_GRAPH_SOURCE_ROOT}/tests/*.cppm"
      "${AZZS_GRAPH_SOURCE_ROOT}/tests/*.cxx"
      "${AZZS_GRAPH_SOURCE_ROOT}/tests/*.h"
      "${AZZS_GRAPH_SOURCE_ROOT}/tests/*.hpp"
      "${AZZS_GRAPH_SOURCE_ROOT}/tests/*.inl"
      "${AZZS_GRAPH_SOURCE_ROOT}/tests/*.ipp"
      "${AZZS_GRAPH_SOURCE_ROOT}/tests/*.ixx")
  endif()
  file(GLOB_RECURSE project_sources LIST_DIRECTORIES FALSE
    ${project_source_patterns})

  foreach(source_path IN LISTS project_sources)
    file(RELATIVE_PATH relative_source
      "${AZZS_GRAPH_SOURCE_ROOT}" "${source_path}")
    string(REPLACE "\\" "/" relative_source "${relative_source}")
    _azzs_matches_any("${relative_source}"
      AZZS_ARCHITECTURE_SCAN_EXCLUDE_PATTERNS source_excluded)
    if(source_excluded)
      continue()
    endif()
    _azzs_source_role("${relative_source}" source_role)
    _azzs_graph_add_node("${relative_source}" "${source_role}")

    file(STRINGS "${source_path}" source_lines)
    foreach(source_line IN LISTS source_lines)
      string(REGEX REPLACE "//.*$" "" code_line "${source_line}")
      if(code_line MATCHES
         "(^|;)[ \\t]*(export[ \\t]+)?import[ \\t]+")
        _azzs_architecture_fail(
          "${relative_source}"
          "${relative_source} (${source_role}) -> ${code_line} (unregistered C++ import)"
          "${source_role} -> registered #include dependencies only")
      endif()
    endforeach()

    file(STRINGS "${source_path}" include_lines
      REGEX "^[ \\t]*#[ \\t]*include")
    foreach(include_line IN LISTS include_lines)
      set(include_kind "")
      if(include_line MATCHES
         "^[ \\t]*#[ \\t]*include[ \\t]*\"([^\"]+)\"[ \\t]*(//.*)?$")
        set(include_path "${CMAKE_MATCH_1}")
        set(include_kind quoted)
      elseif(include_line MATCHES
             "^[ \\t]*#[ \\t]*include[ \\t]*<([^>]+)>[ \\t]*(//.*)?$")
        set(include_path "${CMAKE_MATCH_1}")
        set(include_kind angle)
      else()
        _azzs_architecture_fail(
          "${relative_source}"
          "${relative_source} (${source_role}) -> ${include_line} (dynamic include)"
          "${source_role} -> literal registered headers")
      endif()

      _azzs_resolve_project_include(
        "${source_path}" "${include_path}" "${source_role}" resolved_include)
      if(resolved_include)
        _azzs_project_relative_role(
          "${resolved_include}" relative_include include_role)
        _azzs_graph_add_node("${relative_include}" "${include_role}")
        _azzs_graph_add_edge("${relative_source}" "${relative_include}")
        continue()
      endif()

      string(TOLOWER "${include_path}" normalized_include)
      _azzs_matches_any("${normalized_include}"
        AZZS_ARCHITECTURE_PLATFORM_HEADER_PATTERNS platform_header)
      _azzs_matches_any("${normalized_include}"
        AZZS_ARCHITECTURE_STANDARD_HEADER_PATTERNS standard_header)
      _azzs_matches_any("${include_path}"
        AZZS_ARCHITECTURE_WINUI_GENERATED_HEADER_PATTERNS generated_header)
      if(platform_header)
        _azzs_graph_add_edge(
          "${relative_source}" "external:windows-platform")
      elseif(standard_header AND include_kind STREQUAL "angle")
        _azzs_graph_add_edge("${relative_source}" "external:cpp-runtime")
      elseif(generated_header)
        _azzs_graph_add_edge("${relative_source}" "external:winui-generated")
      else()
        set(unresolved_node "unresolved-include:${include_path}")
        _azzs_graph_add_node("${unresolved_node}" unregistered)
        _azzs_graph_add_edge("${relative_source}" "${unresolved_node}")
      endif()
    endforeach()

    if(source_role IN_LIST AZZS_ARCHITECTURE_NO_DIRECT_SYSTEM_EFFECT_ROLES)
      foreach(source_line IN LISTS source_lines)
        string(REGEX REPLACE "//.*$" "" code_line "${source_line}")
        foreach(effect_pattern IN LISTS
            AZZS_ARCHITECTURE_SYSTEM_EFFECT_PATTERNS)
          if(code_line MATCHES "${effect_pattern}")
            _azzs_architecture_fail(
              "${relative_source}"
              "${relative_source} (${source_role}) -> direct system effect '${CMAKE_MATCH_0}'"
              "${source_role} -> application interfaces for system effects")
          endif()
        endforeach()
      endforeach()
    endif()
  endforeach()
endif()

list(REMOVE_DUPLICATES graph_edges)

foreach(node_key IN LISTS graph_node_keys)
  set("AZZS_GRAPH_INDEGREE_${node_key}" 0)
  set("AZZS_GRAPH_ADJACENCY_${node_key}" "")
endforeach()

foreach(record IN LISTS graph_edges)
  _azzs_parse_pair("${record}" from_name to_name)
  _azzs_graph_key("${from_name}" from_key)
  _azzs_graph_key("${to_name}" to_key)
  if(NOT DEFINED "AZZS_GRAPH_NAME_${from_key}")
    _azzs_architecture_fail(
      "${from_name}" "${from_name} -> ${to_name} (missing source node)"
      "all dependency endpoints must be registered")
  endif()
  if(NOT DEFINED "AZZS_GRAPH_NAME_${to_key}")
    _azzs_architecture_fail(
      "${from_name}" "${from_name} -> ${to_name} (missing target node)"
      "all dependency endpoints must be registered")
  endif()

  list(APPEND "AZZS_GRAPH_ADJACENCY_${from_key}" "${to_key}")
  set(indegree_variable "AZZS_GRAPH_INDEGREE_${to_key}")
  math(EXPR updated_indegree "${${indegree_variable}} + 1")
  set("${indegree_variable}" "${updated_indegree}")
endforeach()

set(ready_nodes)
foreach(node_key IN LISTS graph_node_keys)
  if(AZZS_GRAPH_INDEGREE_${node_key} EQUAL 0)
    list(APPEND ready_nodes "${node_key}")
  endif()
endforeach()

set(processed_count 0)
while(ready_nodes)
  list(POP_FRONT ready_nodes current_key)
  math(EXPR processed_count "${processed_count} + 1")
  foreach(neighbor_key IN LISTS AZZS_GRAPH_ADJACENCY_${current_key})
    set(indegree_variable "AZZS_GRAPH_INDEGREE_${neighbor_key}")
    math(EXPR updated_indegree "${${indegree_variable}} - 1")
    set("${indegree_variable}" "${updated_indegree}")
    if(updated_indegree EQUAL 0)
      list(APPEND ready_nodes "${neighbor_key}")
    endif()
  endforeach()
endwhile()

list(LENGTH graph_node_keys node_count)
if(NOT processed_count EQUAL node_count)
  set_property(GLOBAL PROPERTY AZZS_FOUND_CYCLE "")
  foreach(node_key IN LISTS graph_node_keys)
    if(AZZS_GRAPH_INDEGREE_${node_key} GREATER 0)
      _azzs_find_cycle("${node_key}" "${node_key}" "${node_key}")
      get_property(cycle_keys GLOBAL PROPERTY AZZS_FOUND_CYCLE)
      if(cycle_keys)
        break()
      endif()
    endif()
  endforeach()
  set(cycle_names)
  foreach(cycle_key IN LISTS cycle_keys)
    list(APPEND cycle_names "${AZZS_GRAPH_NAME_${cycle_key}}")
  endforeach()
  string(JOIN " -> " cycle_path ${cycle_names})
  list(GET cycle_names 0 cycle_target)
  _azzs_architecture_fail(
    "${cycle_target}"
    "dependency cycle: ${cycle_path}"
    "graph must be acyclic")
endif()

foreach(record IN LISTS graph_edges)
  _azzs_parse_pair("${record}" from_name to_name)
  _azzs_graph_key("${from_name}" from_key)
  _azzs_graph_key("${to_name}" to_key)
  set(from_role "${AZZS_GRAPH_ROLE_${from_key}}")
  set(to_role "${AZZS_GRAPH_ROLE_${to_key}}")
  if(NOT from_role IN_LIST AZZS_ARCHITECTURE_ROLES)
    _azzs_architecture_fail(
      "${from_name}"
      "${from_name} -> role '${from_role}'"
      "registered roles: ${AZZS_ARCHITECTURE_ROLES}")
  endif()
  set(allowed_variable "AZZS_ARCHITECTURE_ALLOWED_${from_role}")
  set(allowed_roles "${${allowed_variable}}")
  if(NOT to_role IN_LIST allowed_roles)
    _azzs_architecture_fail(
      "${from_name}"
      "${from_name} (${from_role}) -> ${to_name} (${to_role})"
      "${from_role} -> ${allowed_roles}")
  endif()
endforeach()

foreach(node_key IN LISTS graph_node_keys)
  set(node_name "${AZZS_GRAPH_NAME_${node_key}}")
  set(node_role "${AZZS_GRAPH_ROLE_${node_key}}")
  if(NOT node_role IN_LIST AZZS_ARCHITECTURE_ROLES)
    _azzs_architecture_fail(
      "${node_name}"
      "${node_name} -> role '${node_role}'"
      "registered roles: ${AZZS_ARCHITECTURE_ROLES}")
  endif()
  if(node_role STREQUAL "composition" AND
     NOT node_name IN_LIST AZZS_ARCHITECTURE_COMPOSITION_NODES)
    _azzs_architecture_fail(
      "${node_name}"
      "${node_name} -> unregistered composition role"
      "composition -> ${AZZS_ARCHITECTURE_COMPOSITION_NODES}")
  endif()
endforeach()

foreach(node_name IN LISTS declared_graph_node_names)
  _azzs_graph_key("${node_name}" node_key)
  set(node_role "${AZZS_GRAPH_ROLE_${node_key}}")
  if(NOT node_role STREQUAL "platform_adapter" AND
     NOT node_role STREQUAL "infrastructure_adapter")
    continue()
  endif()

  set(has_application_interface FALSE)
  foreach(record IN LISTS graph_edges)
    _azzs_parse_pair("${record}" from_name to_name)
    if(NOT from_name STREQUAL node_name)
      continue()
    endif()
    _azzs_graph_key("${to_name}" to_key)
    if(AZZS_GRAPH_ROLE_${to_key} STREQUAL "application")
      set(has_application_interface TRUE)
      break()
    endif()
  endforeach()
  if(NOT has_application_interface)
    _azzs_architecture_fail(
      "${node_name}"
      "${node_name} (${node_role}) -> no application interface"
      "${node_role} -> at least one application target")
  endif()
endforeach()

list(LENGTH graph_edges edge_count)
message(STATUS
  "Architecture graph accepted: ${node_count} nodes, ${edge_count} edges")
