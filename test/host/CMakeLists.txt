cmake_minimum_required(VERSION 3.16)

# Tier 1 host-native unit tests (DESIGN.md §12.2). Build for the ESP-IDF
# Linux target: idf.py --preview set-target linux build
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_host)
