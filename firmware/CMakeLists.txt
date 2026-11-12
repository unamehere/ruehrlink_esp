cmake_minimum_required(VERSION 3.16)

# Eine Versionsnummer für alles: ../VERSION (siehe docs/updates.md)
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" PROJECT_VER LIMIT_COUNT 1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(freikoch_bruecke)
