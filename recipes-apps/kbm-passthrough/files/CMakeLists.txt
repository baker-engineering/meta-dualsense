cmake_minimum_required(VERSION 3.16)
project(kbm-passthrough C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -O2)

add_executable(kbm-passthrough main.c)

include(GNUInstallDirs)
install(TARGETS kbm-passthrough RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
