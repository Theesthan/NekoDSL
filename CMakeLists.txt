cmake_minimum_required(VERSION 3.15)

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
  message(FATAL_ERROR "Prevented in-tree built. Please create a build directory outside of the neko source code and call cmake from there")
endif()

if (POLICY CMP0048)
  cmake_policy(SET CMP0048 NEW)
endif()

project(neko VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(NEKO_COMPILER "Generate nekoc target"                           ON )
option(NEKO_DOCS     "Generate documentation target (Requires Sphynx)" OFF)
option(NEKO_GEN_STL  "Generate STL files (Requires Python)"            OFF)
option(NEKO_GRAMMAR  "Build grammar (Requires Bison and re2c)"         OFF)
option(NEKO_INSTALL  "Generate install targets"                        OFF)
option(NEKO_TESTS    "Generate test target"                            OFF)

add_subdirectory(dependencies)

add_subdirectory(neko)

if(NEKO_COMPILER)
  add_subdirectory(nekoc)
endif()

if(NEKO_DOCS)
  add_subdirectory(docs)
endif()

add_subdirectory(nekostl)

if(NEKO_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
