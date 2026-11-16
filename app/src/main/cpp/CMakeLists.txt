cmake_minimum_required(VERSION 3.22.1)

project(HelloWorldNativeC LANGUAGES C)

# Use the C11 standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compile main.c into libhello.so
add_library(hello SHARED
    main.c
)

# Link Android native system APIs
target_link_libraries(hello
    PRIVATE
    android
)
