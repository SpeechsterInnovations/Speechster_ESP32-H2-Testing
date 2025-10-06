cmake_minimum_required(VERSION 3.5)

# This file tells CMake which source files to compile for the 'main' component.
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
)
