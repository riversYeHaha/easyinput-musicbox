cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(musicbox)

# 按 ESP-IDF 5.5.x 验证（本机 5.5.5）
if(NOT IDF_VERSION VERSION_EQUAL "5.5.5")
    message(WARNING "本项目按 ESP-IDF 5.5.x 验证；当前构建版本 ${IDF_VERSION}")
endif()
