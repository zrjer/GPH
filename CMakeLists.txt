cmake_minimum_required (VERSION 3.12)
project(GPH LANGUAGES CXX
            DESCRIPTION
            "General-purpose heuristics for mixed-integer programming")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB SOURCES "src/main.cpp" 
                  "src/core/*.cpp" 
                  "src/interfaces/*.cpp"  
                  "src/io/*.cpp" 
                  "src/methods/*.cpp")
               
add_executable(gph ${SOURCES})

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

target_compile_options(gph PUBLIC  "-Wall" "-Wextra" "-Wpedantic")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# dependencies
find_package(TBB REQUIRED tbb tbbmalloc_proxy)
find_package(ZLIB)

if(ZLIB_FOUND)
   add_compile_definitions(ZLIB_FOUND)
endif()

# use the header only version of fmtlib
add_compile_definitions(FMT_HEADER_ONLY)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")

# solvers
if(SOLVER STREQUAL "cplex")
   find_package(Cplex REQUIRED)
elseif(SOLVER STREQUAL "soplex")
   find_package(SOPLEX CONFIG)
elseif(SOLVER STREQUAL "glpk")
   find_package(GLPK)
elseif(SOLVER STREQUAL "")
   message(FATAL_ERROR "LP solver not specified")
else()
   message(FATAL_ERROR "LP solver unknown")
endif()

if(CPLEX_FOUND)
   set(LPSOLVER_INCLUDE_DIRS ${CPLEX_CONCERT_INCLUDE_DIR} ${CPLEX_INCLUDE_DIR})
   set(LPSOLVER_LIBRARIES ${CPLEX_CONCERT_LIBRARY} ${CPLEX_ILOCPLEX_LIBRARY} dl ${CPLEX_LIBRARY} pthread)
   add_compile_definitions(CONCERT_CPLEX_FOUND)
   add_compile_definitions(IL_STD)
elseif(SOPLEX_FOUND)
   set(LPSOLVER_INCLUDE_DIRS ${SOPLEX_INCLUDE_DIRS})
   set(LPSOLVER_LIBRARIES ${SOPLEX_LIBRARIES})
   add_compile_definitions(SOPLEX_FOUND)
elseif(GLPK_FOUND)
   set(LPSOLVER_INCLUDE_DIRS ${GLPK_INCLUDE_DIR})
   set(LPSOLVER_LIBRARIES ${GLPK_LIBRARY})
   add_compile_definitions(GLPK_FOUND)
else()
   message(FATAL_ERROR "LP solver not found")
endif()

target_include_directories(gph PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/external)
target_include_directories(gph SYSTEM PRIVATE ${LPSOLVER_INCLUDE_DIRS} ${ZLIB_INCLUDE_DIRS})
target_link_libraries(gph PUBLIC TBB::tbb TBB::tbbmalloc_proxy ${LPSOLVER_LIBRARIES}  ${ZLIB_LIBRARIES} m stdc++fs)
