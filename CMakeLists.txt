cmake_minimum_required(VERSION 3.16.0)

# Firmware version reported at boot (esp_app_desc). Set explicitly so the build
# does not depend on `git describe` (fails on a repository without commits).
set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(s2-dashboard)
