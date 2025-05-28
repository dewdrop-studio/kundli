
cmake_minimum_required(VERSION 3.10)

project("Kundli" CXX C)
set(CMAKE_CXX_STANDARD 23)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror")


if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_library(kundli
    src/kundli.cpp
)
add_executable(
    pandit
    src/pandit.cpp
)

target_link_libraries(pandit kundli)

target_include_directories(kundli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(pandit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
