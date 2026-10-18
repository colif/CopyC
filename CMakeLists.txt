cmake_minimum_required(VERSION 3.20)
project(Copy_C LANGUAGES CXX)

add_executable(Copy_C WIN32 Copy_C.cpp)
target_compile_features(Copy_C PRIVATE cxx_std_17)
target_compile_definitions(Copy_C PRIVATE UNICODE _UNICODE)
target_link_libraries(Copy_C PRIVATE comctl32 shell32 ole32 uuid)

if(MSVC)
  target_compile_options(Copy_C PRIVATE /W4 /permissive-)
else()
  target_compile_options(Copy_C PRIVATE -Wall -Wextra -Wpedantic)
endif()
