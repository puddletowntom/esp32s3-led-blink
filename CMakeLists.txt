cmake_minimum_required(VERSION 3.16)
# Firmware version: from version.txt at the project root, or the
# PROJECT_VER cache variable passed on the idf.py command line (-D PROJECT_VER=x.y.z).
if(NOT DEFINED PROJECT_VER)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER)
    string(STRIP "${PROJECT_VER}" PROJECT_VER)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(led_blink)
