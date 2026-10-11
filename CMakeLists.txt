cmake_minimum_required(VERSION 3.16)

# Restrict the build to `main` plus its actual transitive REQUIRES, instead
# of the IDF default of configuring every component under
# $IDF_PATH/components (Bluetooth, camera, SD card, ...) when COMPONENTS is
# left unset - that default cost real clean-build time without changing the
# flashed binary (unused components were already dead-code-eliminated at
# link time).
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_usb_bridge)
