cmake_minimum_required(VERSION 3.16)

project(c_window_skin_sample C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(window_skin
    src/window_skin.c
)

target_include_directories(window_skin
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(window_skin_demo
    sample/main.c
    sample/demo_backend.c
)

target_include_directories(window_skin_demo
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/sample
)

target_link_libraries(window_skin_demo
    PRIVATE
        window_skin
        m
)

if(MSVC)
    target_compile_options(window_skin PRIVATE /W4)
    target_compile_options(window_skin_demo PRIVATE /W4)
else()
    target_compile_options(window_skin PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(window_skin_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()
