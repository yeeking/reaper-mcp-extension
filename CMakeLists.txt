cmake_minimum_required(VERSION 3.10)

project(reaper_mcp CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-fPIC -O2)

# set(REAPER_INSTALL_DIR "$ENV{HOME}/Library/Application Support/REAPER/UserPlugins" CACHE PATH "Destination directory for the built REAPER extension")

if(APPLE)
    set(REAPER_INSTALL_DIR "$ENV{HOME}/Library/Application Support/REAPER/UserPlugins" CACHE PATH
"Destination directory for the built REAPER extension")
elseif(WIN32)
    set(REAPER_INSTALL_DIR "$ENV{APPDATA}/REAPER/UserPlugins" CACHE PATH "Destination directory
for the built REAPER extension")
else()
    set(REAPER_INSTALL_DIR "$ENV{HOME}/.config/REAPER/UserPlugins" CACHE PATH "Destination
directory for the built REAPER extension")
endif()


add_library(reaper_mcp SHARED src/reaper_mcp.cpp)

target_include_directories(reaper_mcp PRIVATE lib/WDL/WDL)
target_include_directories(reaper_mcp PRIVATE lib/reaper-sdk/sdk)

set_target_properties(reaper_mcp PROPERTIES
    PREFIX ""                 # <-- removes "lib"
    OUTPUT_NAME "reaper_mcp"
)

add_custom_command(TARGET reaper_mcp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${REAPER_INSTALL_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:reaper_mcp>" "${REAPER_INSTALL_DIR}"
    COMMENT "Copying reaper_mcp to ${REAPER_INSTALL_DIR}"
)
