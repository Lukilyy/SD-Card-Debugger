cmake_minimum_required(VERSION 3.16)

# Register the project-local FatFs override explicitly.  Besides making the
# dependency unambiguous, changing this root file makes already-configured
# build trees rerun component discovery and replace ESP-IDF's built-in FatFs.
list(APPEND EXTRA_COMPONENT_DIRS
     "${CMAKE_CURRENT_LIST_DIR}/components/fatfs")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sticky_sd_card_debugger)
