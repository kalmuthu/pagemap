cmake_minimum_required (VERSION 2.8)
project(pagemap)

set( CMAKE_C_FLAGS  "${CMAKE_C_FLAGS} -std=c11  -Wall -Wextra -pedantic -ansi -fstrict-aliasing")

add_executable(pagemap pagemap.c)

