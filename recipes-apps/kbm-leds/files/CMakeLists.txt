cmake_minimum_required(VERSION 3.16)
project(kbm-leds C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -O2)

add_executable(kbm-leds main.c)
target_link_libraries(kbm-leds PRIVATE m pthread)

include(GNUInstallDirs)
install(TARGETS kbm-leds RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
