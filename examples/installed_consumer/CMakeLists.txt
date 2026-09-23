cmake_minimum_required(VERSION 3.16)
project(logger_installed_consumer LANGUAGES C)
# The pre-1.0 candidate uses ExactVersion matching deliberately.
find_package(Logger 0.9.0 EXACT CONFIG REQUIRED)
add_executable(logger_consumer main.c)
target_compile_features(logger_consumer PRIVATE c_std_11)
target_compile_options(logger_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(logger_consumer PRIVATE Logger::logger)

# Verifies the static package is PIC and can be embedded in a host SDK DSO.
# The plugin owns its own logger; no pointer crosses independent static copies.
add_library(installed_plugin MODULE plugin.c)
target_link_libraries(installed_plugin PRIVATE Logger::logger)
add_executable(plugin_loader plugin_loader.c)
target_link_libraries(plugin_loader PRIVATE ${CMAKE_DL_LIBS})

option(CONSUMER_TEST_CXX "Test C++11 headers/default macros as well" OFF)
if(CONSUMER_TEST_CXX)
  enable_language(CXX)
  add_executable(logger_cpp cpp.cpp)
  set_target_properties(logger_cpp PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
  target_compile_options(logger_cpp PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(logger_cpp PRIVATE Logger::logger)
endif()
