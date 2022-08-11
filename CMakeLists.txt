cmake_minimum_required(VERSION 3.18)
project(fifo_split)
include(${CMAKE_BINARY_DIR}/conan_paths.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(Boost_USE_STATIC_LIBS ON)

# Use new Boost string/list ABI to avoid linking errors: 
add_definitions(-D_GLIBCXX_USE_CXX11_ABI=1)

find_package(Boost 1.77.0 COMPONENTS program_options REQUIRED)

# n.b. adding this as a subdirectory causes our compiler settings to get stuck to match it:
# So I've chosen C++17 to avoid fighting it
add_definitions(-DUNITCONVERT_NO_BACKWARD_COMPATIBLE_NAMESPACE)
add_subdirectory(deps/UnitConvert)

add_executable(fifo-split fifo-split.cpp)
target_link_libraries(fifo-split ${Boost_LIBRARIES} UnitConvert::UnitConvert)
