cmake_minimum_required(VERSION 3.16)
project(dualsense-ffsd C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -O2)

find_package(Threads REQUIRED)

add_executable(dualsense-ffsd
    main.c
    mapper.c
    config.c
    feature_reports.c
)
target_link_libraries(dualsense-ffsd PRIVATE m rt Threads::Threads)

include(GNUInstallDirs)
install(TARGETS dualsense-ffsd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
