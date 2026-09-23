# Called after production target creation. Never exports test-only targets.
include(CMakePackageConfigHelpers)
foreach(_dir LIBDIR INCLUDEDIR BINDIR DATAROOTDIR DOCDIR)
  if(IS_ABSOLUTE "${CMAKE_INSTALL_${_dir}}" OR
     "${CMAKE_INSTALL_${_dir}}" MATCHES "(^|/)\.\.(/|$)")
    message(FATAL_ERROR "CMAKE_INSTALL_${_dir} must be prefix-relative without '..' for relocatable packages")
  endif()
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(include/logger_version.h.in generated/logger_version.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/logger.symbols")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/logger.symbols" _public_names)
set(_map "LOGGER_0.9 {\n  global:\n")
foreach(_name IN LISTS _public_names)
  if(_name MATCHES "^#" OR _name STREQUAL "")
    continue()
  endif()
  if(NOT _name MATCHES "^[a-z][a-z0-9_]+$")
    message(FATAL_ERROR "Invalid explicit ABI name: ${_name}")
  endif()
  string(APPEND _map "    ${_name};\n")
endforeach()
if(LOGGER_ENABLE_LEGACY_FORK_HELPER)
  string(APPEND _map "    logger_fork_reinit;\n")
endif()
string(APPEND _map "  local: *;\n};\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/logger.map" "${_map}")
get_target_property(_logger_kind logger TYPE)
if(_logger_kind STREQUAL "SHARED_LIBRARY")
  set_target_properties(logger PROPERTIES
    VERSION "${PROJECT_VERSION}" SOVERSION "${LOGGER_ABI_VERSION}"
    INSTALL_RPATH "" INSTALL_RPATH_USE_LINK_PATH FALSE)
  target_link_options(logger PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_BINARY_DIR}/logger.map"
    "LINKER:--no-undefined" "LINKER:--no-undefined-version")
  set_property(TARGET logger APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/logger.map")
  set(LOGGER_PACKAGE_KIND "shared")
else()
  set(LOGGER_PACKAGE_KIND "static")
endif()
set_property(TARGET logger PROPERTY INTERFACE_LOGGER_ABI "${LOGGER_ABI_VERSION}")
set_property(TARGET logger APPEND PROPERTY COMPATIBLE_INTERFACE_STRING LOGGER_ABI)
# Sanitizers are verification builds, never silently installed as a release.
set(_install_default ON)
if(NOT LOGGER_SANITIZE STREQUAL "")
  set(_install_default OFF)
endif()
option(LOGGER_ENABLE_INSTALL "Generate relocatable production install/package rules" ${_install_default})
if(LOGGER_ENABLE_INSTALL AND NOT LOGGER_SANITIZE STREQUAL "")
  message(FATAL_ERROR "Sanitizer builds are test-only: set LOGGER_ENABLE_INSTALL=OFF")
endif()
if(LOGGER_ENABLE_INSTALL)
  set(_pkgdir "${CMAKE_INSTALL_LIBDIR}/cmake/Logger")
  configure_package_config_file(cmake/LoggerConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/LoggerConfig.cmake"
    INSTALL_DESTINATION "${_pkgdir}")
  # No promise that arbitrary pre-1.0 snapshots are ABI compatible.
  write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/LoggerConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)
  file(RELATIVE_PATH LOGGER_PC_PREFIX_REL
    "/__logger_prefix__/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/__logger_prefix__")
  set(LOGGER_PC_DEFINITIONS "")
  if(_logger_kind STREQUAL "STATIC_LIBRARY")
    string(APPEND LOGGER_PC_DEFINITIONS " -DLOGGER_STATIC_DEFINE=1")
  endif()
  if(LOGGER_ENABLE_LEGACY_FORK_HELPER)
    string(APPEND LOGGER_PC_DEFINITIONS " -DLOGGER_ENABLE_LEGACY_FORK_HELPER=1")
  endif()
  configure_file(cmake/logger.pc.in logger.pc @ONLY)
  install(TARGETS logger EXPORT LoggerTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Runtime
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime)
  install(FILES include/logger.h include/audit.h include/console.h
    include/logger_export.h "${CMAKE_CURRENT_BINARY_DIR}/generated/logger_version.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/logger" COMPONENT Development)
  if(LOGGER_ENABLE_LEGACY_FORK_HELPER)
    install(FILES include/logger_fork_compat.h
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/logger" COMPONENT Development)
  endif()
  install(EXPORT LoggerTargets FILE LoggerTargets.cmake NAMESPACE Logger::
    DESTINATION "${_pkgdir}" COMPONENT Development)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/LoggerConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/LoggerConfigVersion.cmake"
    DESTINATION "${_pkgdir}" COMPONENT Development)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/logger.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig" COMPONENT Development)
  install(FILES API.md SECURITY.md TESTING.md CHANGELOG.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT Documentation)
  install(DIRECTORY docs/ DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs"
    COMPONENT Documentation FILES_MATCHING PATTERN "*.md" PATTERN "HISTORY*" EXCLUDE)
  install(DIRECTORY examples/installed_consumer/
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/examples/installed_consumer"
    COMPONENT Documentation)
  set(CPACK_GENERATOR "TGZ")
  set(CPACK_PACKAGE_NAME "prod-c-logger")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Host-owned C Logger and Audit, builtin SHA-256, development candidate")
  set(CPACK_PACKAGE_FILE_NAME "prod-c-logger-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-${LOGGER_PACKAGE_KIND}")
  set(CPACK_PACKAGE_CHECKSUM SHA256)
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
  include(CPack)
endif()
