# Registration only for native production installs. Sanitizer suites exercise
# implementation behavior; they are intentionally not shippable packages.
if(LOGGER_ENABLE_INSTALL AND NOT CMAKE_CROSSCOMPILING)
  find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
  find_program(LOGGER_TEST_CXX NAMES c++ g++)
  find_program(LOGGER_TEST_PKG_CONFIG NAMES pkg-config)
  find_program(LOGGER_TEST_NM NAMES nm)
  find_program(LOGGER_TEST_READELF NAMES readelf)
  if(NOT LOGGER_TEST_CXX OR NOT LOGGER_TEST_PKG_CONFIG OR NOT LOGGER_TEST_NM OR NOT LOGGER_TEST_READELF)
    message(FATAL_ERROR "Native packaging tests require c++, pkg-config, nm and readelf (or configure BUILD_TESTING=OFF)")
  endif()
  set(_legacy_args "")
  if(LOGGER_ENABLE_LEGACY_FORK_HELPER)
    list(APPEND _legacy_args --legacy)
  endif()
  add_test(NAME packaging_install_relocate
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/packaging/check_install.py"
      --source "${CMAKE_CURRENT_SOURCE_DIR}" --build "${CMAKE_CURRENT_BINARY_DIR}"
      --artifact "$<TARGET_FILE:logger>" --kind "${LOGGER_PACKAGE_KIND}"
      --cc "${CMAKE_C_COMPILER}" --cxx "${LOGGER_TEST_CXX}"
      --cmake "${CMAKE_COMMAND}" --libdir "${CMAKE_INSTALL_LIBDIR}"
      --includedir "${CMAKE_INSTALL_INCLUDEDIR}" ${_legacy_args})
  set_tests_properties(packaging_install_relocate PROPERTIES LABELS "packaging;installed-consumer" TIMEOUT 180)
  set(_empty_test_guard "")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.26")
    set(_empty_test_guard --no-tests=error)
  endif()
  # The target is created only together with the registered install test.
  add_custom_target(check-package
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure ${_empty_test_guard} -L packaging
    DEPENDS logger WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" USES_TERMINAL)
endif()
